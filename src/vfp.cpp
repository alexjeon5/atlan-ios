// SPDX-License-Identifier: GPL-2.0-or-later
#include "atlan/vfp.hpp"
#include "cpu/arm64/aarch32/guest_key.hpp"
#include "cpu/arm64/aarch32/semantics.hpp"
#include <array>
#include <bit>
#include <cmath>
#include <cstring>
#include <limits>

namespace atlan::vfp {
namespace a32 = uml::cpu::arm64::aarch32;
namespace {
using Kind = Execution::Kind;

Execution unsupported(const char* reason) { return {Kind::unsupported, 0, MemoryAccess::read, false, reason}; }
Execution fault(std::uint32_t address, MemoryAccess access, bool alignment = false) {
    return {Kind::fault, address, access, alignment, nullptr};
}

bool condition_holds(unsigned cond, std::uint32_t nzcv) {
    const bool n = nzcv >> 31 & 1, z = nzcv >> 30 & 1, c = nzcv >> 29 & 1, v = nzcv >> 28 & 1;
    bool result = false;
    switch (cond >> 1) {
    case 0: result = z; break;
    case 1: result = c; break;
    case 2: result = n; break;
    case 3: result = v; break;
    case 4: result = c && !z; break;
    case 5: result = n == v; break;
    case 6: result = n == v && !z; break;
    default: return true;
    }
    return (cond & 1) && cond != 15 ? !result : result;
}

std::byte* bank(CpuState& cpu) { return reinterpret_cast<std::byte*>(cpu.vectors.data()); }
std::uint32_t get_s(CpuState& cpu, unsigned n) { std::uint32_t v; std::memcpy(&v, bank(cpu) + 4 * n, 4); return v; }
void set_s(CpuState& cpu, unsigned n, std::uint32_t v) { std::memcpy(bank(cpu) + 4 * n, &v, 4); }
std::uint64_t get_d(CpuState& cpu, unsigned n) { std::uint64_t v; std::memcpy(&v, bank(cpu) + 8 * n, 8); return v; }
void set_d(CpuState& cpu, unsigned n, std::uint64_t v) { std::memcpy(bank(cpu) + 8 * n, &v, 8); }

template <typename T> T get(CpuState& cpu, unsigned n) {
    if constexpr (sizeof(T) == 4) return std::bit_cast<float>(get_s(cpu, n));
    else return std::bit_cast<double>(get_d(cpu, n));
}
template <typename T> void put(CpuState& cpu, unsigned n, T value) {
    if constexpr (sizeof(T) == 4) set_s(cpu, n, std::bit_cast<std::uint32_t>(value));
    else set_d(cpu, n, std::bit_cast<std::uint64_t>(value));
}

// Rounding mode, flush-to-zero, default NaN, vector length and stride are not
// implemented; arithmetic refuses to run when FPSCR asks for them.
constexpr std::uint32_t unsupported_fpscr_modes = 0x03F7'0000;
constexpr std::uint32_t fpscr_ioc = 1;

template <typename T> [[gnu::noinline]] Execution arithmetic(CpuState& cpu, std::uint32_t w, unsigned d, unsigned n, unsigned m) {
    const bool p = w >> 23 & 1, s = w >> 6 & 1;
    const unsigned opc = w >> 20 & 3;
    const T dv = get<T>(cpu, d), nv = get<T>(cpu, n), mv = get<T>(cpu, m);
    T result;
    if (!p) {
        volatile T product = nv * mv;  // VMLA/VMLS round the product first: never fused
        switch (opc) {
        case 0: result = s ? dv - product : dv + product; break;
        case 1: result = s ? -dv - product : -dv + product; break;
        case 2: result = s ? -product : static_cast<T>(product); break;
        default: result = s ? nv - mv : nv + mv; break;
        }
    } else if (opc == 0 && !s) {
        result = nv / mv;
    } else {
        return unsupported("unallocated VFP data-processing encoding");
    }
    put<T>(cpu, d, result);
    return {Kind::retired};
}

template <typename T> bool is_signaling(T value) {
    if constexpr (sizeof(T) == 4) return std::isnan(value) && !(std::bit_cast<std::uint32_t>(value) & 0x00400000U);
    else return std::isnan(value) && !(std::bit_cast<std::uint64_t>(value) & 0x0008000000000000ULL);
}

template <typename T> Execution compare(CpuState& cpu, std::uint32_t w, unsigned d, unsigned m) {
    const T a = get<T>(cpu, d);
    const T b = (w >> 16 & 15) == 5 ? T{0} : get<T>(cpu, m);
    std::uint32_t flags;
    if (std::isnan(a) || std::isnan(b)) {
        flags = 0x3 << 28;
        if ((w >> 7 & 1) || is_signaling(a) || is_signaling(b)) cpu.fpscr |= fpscr_ioc;
    } else if (a == b) flags = 0x6 << 28;
    else if (a < b) flags = 0x8 << 28;
    else flags = 0x2 << 28;
    cpu.fpscr = (cpu.fpscr & 0x0fffffff) | flags;
    return {Kind::retired};
}

template <typename T> std::uint32_t to_int(T value, bool is_signed, bool round_zero) {
    if (std::isnan(value)) return 0;
    const double v = round_zero ? std::trunc(static_cast<double>(value)) : std::nearbyint(static_cast<double>(value));
    if (is_signed) {
        if (v >= 2147483648.0) return 0x7fffffffU;
        if (v <= -2147483648.0) return 0x80000000U;
        return static_cast<std::uint32_t>(static_cast<std::int32_t>(v));
    }
    if (v >= 4294967296.0) return 0xffffffffU;
    if (v <= 0.0) return 0;
    return static_cast<std::uint32_t>(v);
}

template <typename T> unsigned reg_of(unsigned high, unsigned low) {
    return sizeof(T) == 4 ? high << 1 | low : low << 4 | high;
}

template <typename T> Execution extension(CpuState& cpu, std::uint32_t w) {
    const unsigned vd = w >> 12 & 15, dbit = w >> 22 & 1, vm = w & 15, mbit = w >> 5 & 1;
    const unsigned d = reg_of<T>(vd, dbit), m = reg_of<T>(vm, mbit);
    if (!(w >> 6 & 1)) {  // VMOV (immediate)
        const std::uint32_t imm8 = (w >> 12 & 0xf0) | (w & 15);
        const std::uint64_t sign = imm8 >> 7, b = imm8 >> 6 & 1, low = imm8 & 0x3f;
        if constexpr (sizeof(T) == 4)
            set_s(cpu, d, static_cast<std::uint32_t>(sign << 31 | (b ^ 1) << 30 | (b ? 0x1fU : 0U) << 25 | low << 19));
        else
            set_d(cpu, d, sign << 63 | (b ^ 1) << 62 | (b ? 0xffULL : 0ULL) << 54 | low << 48);
        return {Kind::retired};
    }
    const unsigned opc2 = w >> 16 & 15, op = w >> 7 & 1;
    switch (opc2) {
    case 0:
        if (op == 0) put<T>(cpu, d, get<T>(cpu, m));
        else put<T>(cpu, d, std::fabs(get<T>(cpu, m)));
        return {Kind::retired};
    case 1:
        if (op == 0) put<T>(cpu, d, -get<T>(cpu, m));
        else put<T>(cpu, d, std::sqrt(get<T>(cpu, m)));
        return {Kind::retired};
    case 4: case 5: return compare<T>(cpu, w, d, m);
    case 8: {  // integer to floating point; the source integer is always in an S register
        const unsigned sm = vm << 1 | mbit;
        const std::uint32_t raw = get_s(cpu, sm);
        put<T>(cpu, d, op ? static_cast<T>(static_cast<std::int32_t>(raw)) : static_cast<T>(raw));
        return {Kind::retired};
    }
    case 12: case 13: {  // floating point to integer; the result is always in an S register
        const unsigned sd = vd << 1 | dbit;
        set_s(cpu, sd, to_int<T>(get<T>(cpu, m), opc2 == 13, op != 0));
        return {Kind::retired};
    }
    default: return unsupported("VFP extension encoding (fixed-point, half-precision, rounding variants)");
    }
}

Execution transfer(CpuState& cpu, Memory& memory, std::uint32_t w, std::uint32_t pc) {
    const unsigned cp = w >> 8 & 15, rn = w >> 16 & 15, vd = w >> 12 & 15, dbit = w >> 22 & 1;
    const bool p = w >> 24 & 1, u = w >> 23 & 1, wb = w >> 21 & 1, load = w >> 20 & 1;
    const std::uint32_t imm = (w & 0xff) * 4;
    const bool dbl = cp == 11;
    if (!(p && !wb) && !(!p && u) && !(p && !u && wb))
        return unsupported("unsupported VFP load/store addressing form");
    std::uint32_t base = rn == 15 ? (pc + 8) & ~3U : static_cast<std::uint32_t>(cpu.registers[rn]);
    std::uint32_t address, count;
    if (p && !wb) {  // VLDR / VSTR
        address = u ? base + imm : base - imm;
        count = 1;
    } else {
        if (rn == 15 && wb) return unsupported("VLDM/VSTM writeback to PC");
        count = (w & 0xff) / (dbl ? 2 : 1);
        if (dbl && (w & 1)) return unsupported("FLDMX/FSTMX");
        if (count == 0 || (dbl ? count > 16 : count > 32)) return unsupported("VLDM/VSTM register count");
        const std::uint32_t bytes = (w & 0xff) * 4;
        address = u ? base : base - bytes;
        if (wb) cpu.registers[rn] = u ? base + bytes : base - bytes;
    }
    if (address % 4) return fault(address, load ? MemoryAccess::read : MemoryAccess::write, true);
    const unsigned first = dbl ? (dbit << 4 | vd) : (vd << 1 | dbit);
    const std::uint32_t unit = dbl ? 8 : 4;
    if ((first + count) * unit > sizeof(cpu.vectors))
        return unsupported("VLDM/VSTM register range past the register file");
    for (unsigned i = 0; i < count; ++i) {
        const std::uint32_t at = address + i * unit;
        std::byte* slot = bank(cpu) + (first + i) * unit;
        if (load) {
            if (!memory.read(at, std::span<std::byte>(slot, unit))) {
                return fault(at, MemoryAccess::read);
            }
        } else if (!memory.write(at, std::span<const std::byte>(slot, unit))) {
            return fault(at, MemoryAccess::write);
        }
    }
    return {Kind::retired};
}

// ---- Pre-decoded handlers for the block cache (prepare_fast) ----
// Each mirrors one path of execute_a32() above with the fields of the word resolved when the block is built.
// A handler that cannot finish without an event returns fast_fallback before changing any state.
using a32::FastOp;
using a32::fast_fallback;
using a32::fast_next;

inline bool passes(const CpuState& cpu, const FastOp& op) { return (op.cond_mask >> (cpu.nzcv >> 28) & 1U) != 0U; }
// execute_a32() moves the PC and leaves ITSTATE alone.
inline unsigned next(CpuState&, const FastOp&) { return fast_next; }  // the block runner writes back the PC
inline bool modes_unsupported(const CpuState& cpu) { return (cpu.fpscr & unsupported_fpscr_modes) != 0; }

// arithmetic<T>() itself, kept out of line: a NaN operand's payload reaches the result by operand order, which
// a second copy of the expressions could compile differently. `imm` is the word.
template <typename T> unsigned fast_arithmetic(CpuState& cpu, const FastOp& op, Memory&) noexcept {
    if (!passes(cpu, op)) return next(cpu, op);
    if (modes_unsupported(cpu)) return fast_fallback;
    (void)arithmetic<T>(cpu, op.imm, op.d, op.n, op.m);
    return next(cpu, op);
}
// VMOV (register), VABS, VNEG, VSQRT.
template <typename T, int Which> unsigned fast_unary(CpuState& cpu, const FastOp& op, Memory&) noexcept {
    if (!passes(cpu, op)) return next(cpu, op);
    if (modes_unsupported(cpu)) return fast_fallback;
    const T value = get<T>(cpu, op.m);
    put<T>(cpu, op.d, Which == 0 ? value : Which == 1 ? std::fabs(value) : Which == 2 ? -value : std::sqrt(value));
    return next(cpu, op);
}
// VMOV (immediate): the expanded constant is in `key`.
template <typename T> unsigned fast_constant(CpuState& cpu, const FastOp& op, Memory&) noexcept {
    if (!passes(cpu, op)) return next(cpu, op);
    if (modes_unsupported(cpu)) return fast_fallback;
    if constexpr (sizeof(T) == 4) set_s(cpu, op.d, static_cast<std::uint32_t>(op.key));
    else set_d(cpu, op.d, op.key);
    return next(cpu, op);
}
// VCMP and VCMPE, with a register or with zero: compare<T>() reads those two choices from the word in `imm`.
template <typename T> unsigned fast_compare(CpuState& cpu, const FastOp& op, Memory&) noexcept {
    if (!passes(cpu, op)) return next(cpu, op);
    if (modes_unsupported(cpu)) return fast_fallback;
    (void)compare<T>(cpu, op.imm, op.d, op.m);
    return next(cpu, op);
}
template <typename T, bool Signed> unsigned fast_from_integer(CpuState& cpu, const FastOp& op, Memory&) noexcept {
    if (!passes(cpu, op)) return next(cpu, op);
    if (modes_unsupported(cpu)) return fast_fallback;
    const std::uint32_t raw = get_s(cpu, op.m);
    put<T>(cpu, op.d, Signed ? static_cast<T>(static_cast<std::int32_t>(raw)) : static_cast<T>(raw));
    return next(cpu, op);
}
template <typename T, bool Signed, bool RoundZero> unsigned fast_to_integer(CpuState& cpu, const FastOp& op, Memory&) noexcept {
    if (!passes(cpu, op)) return next(cpu, op);
    if (modes_unsupported(cpu)) return fast_fallback;
    set_s(cpu, op.d, to_int<T>(get<T>(cpu, op.m), Signed, RoundZero));
    return next(cpu, op);
}
// VCVT between single and double precision.
template <bool FromDouble> unsigned fast_convert(CpuState& cpu, const FastOp& op, Memory&) noexcept {
    if (!passes(cpu, op)) return next(cpu, op);
    if (modes_unsupported(cpu)) return fast_fallback;
    if (FromDouble) set_s(cpu, op.d, std::bit_cast<std::uint32_t>(static_cast<float>(get<double>(cpu, op.m))));
    else set_d(cpu, op.d, std::bit_cast<std::uint64_t>(static_cast<double>(get<float>(cpu, op.m))));
    return next(cpu, op);
}
// Core register transfers: `d` is the core register (rt), `n` the second core register, `m` the S or D register.
template <bool ToCore> unsigned fast_move_single(CpuState& cpu, const FastOp& op, Memory&) noexcept {
    if (passes(cpu, op)) {
        if (ToCore) cpu.registers[op.d] = get_s(cpu, op.m);
        else set_s(cpu, op.m, static_cast<std::uint32_t>(cpu.registers[op.d]));
    }
    return next(cpu, op);
}
// VMRS (0: to APSR_nzcv, 1: to a core register) and VMSR (2).
template <int Which> unsigned fast_status(CpuState& cpu, const FastOp& op, Memory&) noexcept {
    if (passes(cpu, op)) {
        if (Which == 0) cpu.nzcv = (cpu.nzcv & 0x0fffffffU) | (cpu.fpscr & 0xf0000000U);
        else if (Which == 1) cpu.registers[op.d] = cpu.fpscr;
        else cpu.fpscr = static_cast<std::uint32_t>(cpu.registers[op.d]);
    }
    return next(cpu, op);
}
template <bool Double, bool ToCore> unsigned fast_move_pair(CpuState& cpu, const FastOp& op, Memory&) noexcept {
    if (passes(cpu, op)) {
        if (Double) {
            if (ToCore) { cpu.registers[op.d] = get_d(cpu, op.m) & 0xffffffffU; cpu.registers[op.n] = get_d(cpu, op.m) >> 32; }
            else set_d(cpu, op.m, (cpu.registers[op.n] & 0xffffffffULL) << 32 | (cpu.registers[op.d] & 0xffffffffULL));
        } else if (ToCore) {
            cpu.registers[op.d] = get_s(cpu, op.m);
            cpu.registers[op.n] = get_s(cpu, op.m + 1U);
        } else {
            set_s(cpu, op.m, static_cast<std::uint32_t>(cpu.registers[op.d]));
            set_s(cpu, op.m + 1U, static_cast<std::uint32_t>(cpu.registers[op.n]));
        }
    }
    return next(cpu, op);
}
// transfer(): `m` is the first S or D register, `shift_n` the register count, `imm` the offset of the first
// byte from the base (`imm2` when BaseConstant), `key` the change of the base when `wback`. Only when the whole
// block is word aligned, inside one page and translates; anything else is left to execute_a32().
template <unsigned Unit, bool Load, bool BaseConstant> unsigned fast_transfer(CpuState& cpu, const FastOp& op, Memory& memory) noexcept {
    if (!passes(cpu, op)) return next(cpu, op);
    const std::uint32_t base = BaseConstant ? op.imm2 : static_cast<std::uint32_t>(cpu.registers[op.n]);
    const std::uint32_t address = base + op.imm;
    const std::uint32_t bytes = Unit * op.shift_n;
    if ((address & 3U) != 0 || (address & (Memory::page_size - 1U)) > Memory::page_size - bytes) return fast_fallback;
    std::byte* host = memory.translate(address, Load ? MemoryAccess::read : MemoryAccess::write);
    if (host == nullptr) return fast_fallback;
    if (!BaseConstant && op.wback) cpu.registers[op.n] = base + static_cast<std::uint32_t>(op.key);
    std::byte* slot = bank(cpu) + op.m * Unit;
    if (Load) std::memcpy(slot, host, bytes);
    else std::memcpy(host, slot, bytes);
    return next(cpu, op);
}
template <bool Load, bool BaseConstant> FastOp::Run fast_transfer_for(bool dbl) {
    return dbl ? static_cast<FastOp::Run>(fast_transfer<8, Load, BaseConstant>) : static_cast<FastOp::Run>(fast_transfer<4, Load, BaseConstant>);
}
constexpr std::uint16_t fast_condition_mask(unsigned cond) {
    std::uint16_t mask = 0;
    for (unsigned flags = 0; flags < 16; ++flags) {
        const bool n = flags & 8, z = flags & 4, c = flags & 2, v = flags & 1;
        bool result = true;
        switch (cond >> 1) {
        case 0: result = z; break;
        case 1: result = c; break;
        case 2: result = n; break;
        case 3: result = v; break;
        case 4: result = c && !z; break;
        case 5: result = n == v; break;
        case 6: result = n == v && !z; break;
        default: break;
        }
        if (cond >> 1 != 7 && (cond & 1)) result = !result;
        if (result) mask = static_cast<std::uint16_t>(mask | 1U << flags);
    }
    return mask;
}
}  // namespace

Execution execute_a32(CpuState& cpu, Memory& memory, std::uint32_t w, std::uint32_t pc) noexcept {
    const unsigned cls = w >> 24 & 15, cp = w >> 8 & 15;
    if (w >> 28 == 15 || (cp != 10 && cp != 11) || cls < 12 || cls > 14) return {};
    const bool dbl = cp == 11;
    Execution out;
    const bool executes = condition_holds(w >> 28, cpu.nzcv);
    if (!executes) {
        out.kind = Kind::retired;
    } else if (cls == 14 && !(w >> 4 & 1)) {  // data processing
        if (cpu.fpscr & unsupported_fpscr_modes) return unsupported("FPSCR rounding, flush-to-zero, default-NaN or vector mode is set");
        const unsigned vd = w >> 12 & 15, dbit = w >> 22 & 1, vn = w >> 16 & 15, nbit = w >> 7 & 1,
                       vm = w & 15, mbit = w >> 5 & 1;
        const bool ext = (w >> 23 & 1) && (w >> 20 & 3) == 3;
        if (ext && (w >> 6 & 1) && (w >> 16 & 15) == 7) {
            if (!(w >> 7 & 1)) return unsupported("unallocated VFP conversion encoding");
            if (dbl) set_s(cpu, vd << 1 | dbit, std::bit_cast<std::uint32_t>(static_cast<float>(get<double>(cpu, reg_of<double>(vm, mbit)))));
            else set_d(cpu, reg_of<double>(vd, dbit), std::bit_cast<std::uint64_t>(static_cast<double>(get<float>(cpu, vm << 1 | mbit))));
            out.kind = Kind::retired;
        } else if (ext) {
            out = dbl ? extension<double>(cpu, w) : extension<float>(cpu, w);
        } else if (dbl) {
            out = arithmetic<double>(cpu, w, reg_of<double>(vd, dbit), reg_of<double>(vn, nbit), reg_of<double>(vm, mbit));
        } else {
            out = arithmetic<float>(cpu, w, reg_of<float>(vd, dbit), reg_of<float>(vn, nbit), reg_of<float>(vm, mbit));
        }
    } else if (cls == 14) {  // register transfers
        const unsigned rt = w >> 12 & 15, sel = w >> 21 & 7, load = w >> 20 & 1;
        if (sel == 0 && !dbl) {
            const unsigned sn = (w >> 16 & 15) << 1 | (w >> 7 & 1);
            if (rt == 15) return unsupported("VMOV with PC as the core register");
            if (load) cpu.registers[rt] = get_s(cpu, sn); else set_s(cpu, sn, static_cast<std::uint32_t>(cpu.registers[rt]));
            out.kind = Kind::retired;
        } else if (sel == 7) {
            if ((w >> 16 & 15) != 1) return unsupported("VMRS/VMSR of a register other than FPSCR");
            if (load) {
                if (rt == 15) cpu.nzcv = (cpu.nzcv & 0x0fffffffU) | (cpu.fpscr & 0xf0000000U);
                else cpu.registers[rt] = cpu.fpscr;
            } else {
                cpu.fpscr = static_cast<std::uint32_t>(cpu.registers[rt]);
            }
            out.kind = Kind::retired;
        } else {
            return unsupported("VMOV between core registers and scalar elements");
        }
    } else if ((w >> 21 & 0x7f) == 0x62) {  // VMOV two core registers <-> two singles or one double
        const unsigned rt = w >> 12 & 15, rt2 = w >> 16 & 15, load = w >> 20 & 1;
        if (rt == 15 || rt2 == 15) return unsupported("VMOV with PC as a core register");
        if (dbl) {
            const unsigned m = (w >> 5 & 1) << 4 | (w & 15);
            if (load) { cpu.registers[rt] = get_d(cpu, m) & 0xffffffffU; cpu.registers[rt2] = get_d(cpu, m) >> 32; }
            else set_d(cpu, m, (cpu.registers[rt2] & 0xffffffffULL) << 32 | (cpu.registers[rt] & 0xffffffffULL));
        } else {
            const unsigned m = (w & 15) << 1 | (w >> 5 & 1);
            if (m == 31) return unsupported("VMOV pair past S31");
            if (load) { cpu.registers[rt] = get_s(cpu, m); cpu.registers[rt2] = get_s(cpu, m + 1); }
            else { set_s(cpu, m, static_cast<std::uint32_t>(cpu.registers[rt])); set_s(cpu, m + 1, static_cast<std::uint32_t>(cpu.registers[rt2])); }
        }
        out.kind = Kind::retired;
    } else {
        out = transfer(cpu, memory, w, pc);
    }
    if (out.kind == Kind::retired) cpu.pc = a32::a32_key(pc + 4);
    return out;
}
}

namespace atlan::vfp {
bool prepare_fast(std::uint32_t w, std::uint32_t pc, a32::FastOp& out) noexcept {
    const unsigned cls = w >> 24 & 15, cp = w >> 8 & 15;
    if (w >> 28 == 15 || (cp != 10 && cp != 11) || cls < 12 || cls > 14) return false;
    const bool dbl = cp == 11;
    FastOp op{};
    op.next_key = a32::a32_key(pc + 4);
    op.cond_mask = fast_condition_mask(w >> 28);
    op.imm = w;
    const unsigned vd = w >> 12 & 15, dbit = w >> 22 & 1, vn = w >> 16 & 15, nbit = w >> 7 & 1, vm = w & 15, mbit = w >> 5 & 1;
    const auto single = [](unsigned high, unsigned low) { return static_cast<std::uint8_t>(high << 1 | low); };
    const auto wide = [](unsigned high, unsigned low) { return static_cast<std::uint8_t>(low << 4 | high); };
    if (cls == 14 && !(w >> 4 & 1)) {  // data processing
        const bool ext = (w >> 23 & 1) && (w >> 20 & 3) == 3;
        if (ext && (w >> 6 & 1) && (w >> 16 & 15) == 7) {
            if (!(w >> 7 & 1)) return false;
            op.d = dbl ? single(vd, dbit) : wide(vd, dbit);
            op.m = dbl ? wide(vm, mbit) : single(vm, mbit);
            op.run = dbl ? fast_convert<true> : fast_convert<false>;
        } else if (ext) {
            op.d = dbl ? wide(vd, dbit) : single(vd, dbit);
            op.m = dbl ? wide(vm, mbit) : single(vm, mbit);
            const unsigned opc2 = w >> 16 & 15, bit7 = w >> 7 & 1;
            if (!(w >> 6 & 1)) {
                const std::uint32_t imm8 = (w >> 12 & 0xf0) | (w & 15);
                const std::uint64_t sign = imm8 >> 7, b = imm8 >> 6 & 1, low = imm8 & 0x3f;
                if (dbl) op.key = sign << 63 | (b ^ 1) << 62 | (b ? 0xffULL : 0ULL) << 54 | low << 48;
                else op.key = static_cast<std::uint32_t>(sign << 31 | (b ^ 1) << 30 | (b ? 0x1fU : 0U) << 25 | low << 19);
                op.run = dbl ? fast_constant<double> : fast_constant<float>;
            } else if (opc2 == 0) {
                op.run = dbl ? (bit7 ? fast_unary<double, 1> : fast_unary<double, 0>) : (bit7 ? fast_unary<float, 1> : fast_unary<float, 0>);
            } else if (opc2 == 1) {
                op.run = dbl ? (bit7 ? fast_unary<double, 3> : fast_unary<double, 2>) : (bit7 ? fast_unary<float, 3> : fast_unary<float, 2>);
            } else if (opc2 == 4 || opc2 == 5) {
                op.run = dbl ? fast_compare<double> : fast_compare<float>;
            } else if (opc2 == 8) {
                op.m = single(vm, mbit);
                op.run = dbl ? (bit7 ? fast_from_integer<double, true> : fast_from_integer<double, false>)
                             : (bit7 ? fast_from_integer<float, true> : fast_from_integer<float, false>);
            } else if (opc2 == 12 || opc2 == 13) {
                op.d = single(vd, dbit);
                const bool is_signed = opc2 == 13;
                if (dbl) op.run = is_signed ? (bit7 ? fast_to_integer<double, true, true> : fast_to_integer<double, true, false>)
                                            : (bit7 ? fast_to_integer<double, false, true> : fast_to_integer<double, false, false>);
                else op.run = is_signed ? (bit7 ? fast_to_integer<float, true, true> : fast_to_integer<float, true, false>)
                                        : (bit7 ? fast_to_integer<float, false, true> : fast_to_integer<float, false, false>);
            } else {
                return false;
            }
        } else {
            const unsigned p = w >> 23 & 1, s = w >> 6 & 1, opc = w >> 20 & 3;
            if (p && !(opc == 0 && !s)) return false;
            op.d = dbl ? wide(vd, dbit) : single(vd, dbit);
            op.n = dbl ? wide(vn, nbit) : single(vn, nbit);
            op.m = dbl ? wide(vm, mbit) : single(vm, mbit);
            op.run = dbl ? fast_arithmetic<double> : fast_arithmetic<float>;
        }
    } else if (cls == 14) {  // register transfers
        const unsigned rt = w >> 12 & 15, sel = w >> 21 & 7, load = w >> 20 & 1;
        op.d = static_cast<std::uint8_t>(rt);
        if (sel == 0 && !dbl) {
            if (rt == 15) return false;
            op.m = single(vn, nbit);
            op.run = load ? fast_move_single<true> : fast_move_single<false>;
        } else if (sel == 7) {
            if ((w >> 16 & 15) != 1) return false;
            // A VMSR from the PC would read registers[15]; leave that to execute_a32().
            if (!load && rt == 15) return false;
            op.run = load ? (rt == 15 ? fast_status<0> : fast_status<1>) : fast_status<2>;
        } else {
            return false;
        }
    } else if ((w >> 21 & 0x7f) == 0x62) {  // two core registers <-> two singles or one double
        const unsigned rt = w >> 12 & 15, rt2 = w >> 16 & 15, load = w >> 20 & 1;
        if (rt == 15 || rt2 == 15) return false;
        op.d = static_cast<std::uint8_t>(rt);
        op.n = static_cast<std::uint8_t>(rt2);
        if (dbl) {
            op.m = static_cast<std::uint8_t>((w >> 5 & 1) << 4 | (w & 15));
            op.run = load ? fast_move_pair<true, true> : fast_move_pair<true, false>;
        } else {
            op.m = static_cast<std::uint8_t>((w & 15) << 1 | (w >> 5 & 1));
            if (op.m == 31) return false;
            op.run = load ? fast_move_pair<false, true> : fast_move_pair<false, false>;
        }
    } else {  // VLDR, VSTR, VLDM, VSTM
        const unsigned rn = w >> 16 & 15;
        const bool p = w >> 24 & 1, u = w >> 23 & 1, wb = w >> 21 & 1, load = w >> 20 & 1;
        const std::uint32_t imm = (w & 0xff) * 4;
        if (!(p && !wb) && !(!p && u) && !(p && !u && wb)) return false;
        std::uint32_t count = 1;
        if (p && !wb) {
            op.imm = u ? imm : 0U - imm;
        } else {
            if (rn == 15 && wb) return false;
            count = (w & 0xff) / (dbl ? 2 : 1);
            if (dbl && (w & 1)) return false;
            if (count == 0 || (dbl ? count > 16 : count > 32)) return false;
            op.imm = u ? 0U : 0U - imm;
            op.wback = wb;
            op.key = u ? imm : 0U - imm;
        }
        const unsigned first = dbl ? (dbit << 4 | vd) : (vd << 1 | dbit);
        if ((first + count) * (dbl ? 8U : 4U) > sizeof(CpuState{}.vectors)) return false;
        op.m = static_cast<std::uint8_t>(first);
        op.shift_n = static_cast<std::uint8_t>(count);
        op.n = static_cast<std::uint8_t>(rn);
        op.imm2 = (pc + 8) & ~3U;
        if (rn == 15) op.run = load ? fast_transfer_for<true, true>(dbl) : fast_transfer_for<false, true>(dbl);
        else op.run = load ? fast_transfer_for<true, false>(dbl) : fast_transfer_for<false, false>(dbl);
    }
    if (op.run == nullptr) return false;
    out = op;
    return true;
}
}
