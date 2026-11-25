// SPDX-License-Identifier: GPL-2.0-or-later
//
// Specialised handlers for the commonest pre-decoded instructions
// (semantics.hpp, prepare_fast). Added for the block cache; it is not part of
// the imported Kernarium sources. Each handler mirrors the family handler in
// semantics_integer.cpp, semantics_memory.cpp or semantics_control.cpp for one
// operation and operand shape, with the operands reduced to a FastOp when the
// block is built. A handler that cannot complete on its fast path (a fault, an
// access that crosses a page) returns fast_fallback before it changes any
// state, and the caller runs the instruction through execute_a32(), which
// reports the event. tests/block_cache_tests.cpp compares both paths.

#include "cpu/arm64/aarch32/semantics_internal.hpp"

#include <array>

namespace uml::cpu::arm64::aarch32 {
namespace detail {
namespace {

using ::atlan::Memory;
using ::atlan::MemoryAccess;

/* Bit (N << 3 | Z << 2 | C << 1 | V) of entry `cond` is ConditionHolds(). */
constexpr std::uint16_t condition_mask(unsigned cond) noexcept
{
	std::uint16_t mask = 0;
	for (unsigned flags = 0; flags < 16U; ++flags) {
		const bool n = (flags & 8U) != 0U, z = (flags & 4U) != 0U;
		const bool c = (flags & 2U) != 0U, v = (flags & 1U) != 0U;
		bool result;
		switch (cond >> 1) {
		case 0: result = z; break;
		case 1: result = c; break;
		case 2: result = n; break;
		case 3: result = v; break;
		case 4: result = c && !z; break;
		case 5: result = n == v; break;
		case 6: result = n == v && !z; break;
		default: result = true; break;
		}
		if (cond >> 1 != 7U && (cond & 1U) != 0U)
			result = !result;
		if (result)
			mask = static_cast<std::uint16_t>(mask | 1U << flags);
	}
	return mask;
}

[[nodiscard]] inline bool passes(const CpuState &cpu, const FastOp &op) noexcept
{
	return (op.cond_mask >> (cpu.nzcv >> 28) & 1U) != 0U;
}

[[nodiscard]] inline unsigned advance(CpuState &cpu, const FastOp &op) noexcept
{
	cpu.pc = op.next_key;
	cpu.a32_itstate = op.next_it;
	return fast_next;
}

[[nodiscard]] inline unsigned jump(CpuState &cpu, const FastOp &op, std::uint64_t key) noexcept
{
	cpu.pc = key;
	cpu.a32_itstate = op.next_it;
	return fast_jump;
}

[[nodiscard]] inline std::uint32_t reg(const CpuState &cpu, unsigned n) noexcept
{
	return static_cast<std::uint32_t>(cpu.registers[n]);
}

[[nodiscard]] inline bool flag_c(const CpuState &cpu) noexcept
{
	return (cpu.nzcv >> 29 & 1U) != 0U;
}

inline void set_nzcv(CpuState &cpu, std::uint32_t result, bool c, bool v) noexcept
{
	cpu.nzcv = (cpu.nzcv & 0x0fff'ffffU) | (result & 0x8000'0000U)
		| (result == 0U ? 0x4000'0000U : 0U) | (c ? 0x2000'0000U : 0U)
		| (v ? 0x1000'0000U : 0U);
}

inline void set_nzc(CpuState &cpu, std::uint32_t result, bool c) noexcept
{
	cpu.nzcv = (cpu.nzcv & 0x1fff'ffffU) | (result & 0x8000'0000U)
		| (result == 0U ? 0x4000'0000U : 0U) | (c ? 0x2000'0000U : 0U);
}

constexpr bool is_logical(Op op) noexcept
{
	switch (op) {
	case Op::and_: case Op::bic: case Op::eor: case Op::mov: case Op::mvn:
	case Op::orn: case Op::orr: case Op::teq: case Op::tst:
		return true;
	default:
		return false;
	}
}

constexpr bool is_compare(Op op) noexcept
{
	return op == Op::cmn || op == Op::cmp || op == Op::teq || op == Op::tst;
}

/* The second operand's shape: an immediate, a register, a register shifted by
 * an immediate. */
enum : int { shape_immediate, shape_register, shape_shifted };

/* exec_data_processing() for one operation, shape and flag setting. */
template <Op Operation, int Shape, bool SetFlags>
unsigned data_processing(CpuState &cpu, const FastOp &op, Memory &) noexcept
{
	if (!passes(cpu, op)) [[unlikely]]
		return advance(cpu, op);
	std::uint32_t b;
	bool shifter_carry;
	if (Shape == shape_immediate) {
		b = op.imm;
		shifter_carry = op.carry == 0U ? flag_c(cpu) : op.carry == 2U;
	} else if (Shape == shape_register) {
		b = reg(cpu, op.m);
		shifter_carry = flag_c(cpu);
	} else {
		const auto shifted = shift_c(reg(cpu, op.m), static_cast<SRType>(op.shift_t),
			op.shift_n, flag_c(cpu));
		b = shifted.value;
		shifter_carry = shifted.carry;
	}
	const std::uint32_t a = Operation != Op::mov && Operation != Op::mvn ? reg(cpu, op.n) : 0U;
	std::uint32_t result;
	bool carry = shifter_carry;
	bool overflow = false;
	const auto add = [&](std::uint32_t first, std::uint32_t second, bool carry_in) {
		const auto sum = add_with_carry(first, second, carry_in);
		result = sum.value; carry = sum.carry; overflow = sum.overflow;
	};
	switch (Operation) {
	case Op::add: case Op::addw: case Op::cmn: add(a, b, false); break;
	case Op::adc: add(a, b, flag_c(cpu)); break;
	case Op::sub: case Op::subw: case Op::cmp: add(a, ~b, true); break;
	case Op::sbc: add(a, ~b, flag_c(cpu)); break;
	case Op::rsb: add(~a, b, true); break;
	case Op::rsc: add(~a, b, flag_c(cpu)); break;
	case Op::and_: case Op::tst: result = a & b; break;
	case Op::eor: case Op::teq: result = a ^ b; break;
	case Op::orr: result = a | b; break;
	case Op::orn: result = a | ~b; break;
	case Op::bic: result = a & ~b; break;
	case Op::mov: result = b; break;
	default: result = ~b; break;
	}
	if (!is_compare(Operation))
		cpu.registers[op.d] = result;
	if (SetFlags) {
		if (is_logical(Operation))
			set_nzc(cpu, result, carry);
		else
			set_nzcv(cpu, result, carry, overflow);
	}
	return advance(cpu, op);
}

template <int Shape, bool SetFlags>
[[nodiscard]] FastOp::Run data_processing_for(Op op) noexcept
{
	switch (op) {
#define UML_A32_FAST_DP(name) case Op::name: return data_processing<Op::name, Shape, SetFlags>;
	UML_A32_FAST_DP(add) UML_A32_FAST_DP(addw) UML_A32_FAST_DP(cmn) UML_A32_FAST_DP(adc)
	UML_A32_FAST_DP(sub) UML_A32_FAST_DP(subw) UML_A32_FAST_DP(cmp) UML_A32_FAST_DP(sbc)
	UML_A32_FAST_DP(rsb) UML_A32_FAST_DP(rsc) UML_A32_FAST_DP(and_) UML_A32_FAST_DP(tst)
	UML_A32_FAST_DP(eor) UML_A32_FAST_DP(teq) UML_A32_FAST_DP(orr) UML_A32_FAST_DP(orn)
	UML_A32_FAST_DP(bic) UML_A32_FAST_DP(mov) UML_A32_FAST_DP(mvn)
#undef UML_A32_FAST_DP
	default: return nullptr;
	}
}

/* MOVW, and ADR to a register other than the PC: a constant. */
unsigned move_constant(CpuState &cpu, const FastOp &op, Memory &) noexcept
{
	if (passes(cpu, op)) [[likely]]
		cpu.registers[op.d] = op.imm;
	return advance(cpu, op);
}

unsigned move_top(CpuState &cpu, const FastOp &op, Memory &) noexcept
{
	if (passes(cpu, op)) [[likely]]
		cpu.registers[op.d] = (reg(cpu, op.d) & 0xffffU) | op.imm;
	return advance(cpu, op);
}

/* An IT or a NOP: only the PC and ITSTATE move. */
unsigned nothing(CpuState &cpu, const FastOp &op, Memory &) noexcept
{
	return advance(cpu, op);
}

/* SXTB, SXTH, UXTB and UXTH. */
template <unsigned Bits, bool Sign>
unsigned extend(CpuState &cpu, const FastOp &op, Memory &) noexcept
{
	if (passes(cpu, op)) [[likely]] {
		const std::uint32_t rotated = std::rotr(reg(cpu, op.m), static_cast<int>(op.shift_n));
		std::uint32_t result;
		if (Bits == 8U)
			result = Sign ? static_cast<std::uint32_t>(static_cast<std::int32_t>(
				static_cast<std::int8_t>(rotated & 0xffU))) : rotated & 0xffU;
		else
			result = Sign ? static_cast<std::uint32_t>(static_cast<std::int32_t>(
				static_cast<std::int16_t>(rotated & 0xffffU))) : rotated & 0xffffU;
		cpu.registers[op.d] = result;
	}
	return advance(cpu, op);
}

template <bool SetFlags>
unsigned multiply(CpuState &cpu, const FastOp &op, Memory &) noexcept
{
	if (passes(cpu, op)) [[likely]] {
		const std::uint32_t result = reg(cpu, op.n) * reg(cpu, op.m);
		cpu.registers[op.d] = result;
		if (SetFlags)
			set_nzc(cpu, result, flag_c(cpu));
	}
	return advance(cpu, op);
}

/* How a single transfer forms its address: a constant (the literal pool), the
 * base and an immediate, the base and a register, the base and a shifted
 * register. */
enum : int { address_constant, address_immediate, address_register, address_shifted };

/* exec_single_transfer() for the byte, halfword and word loads and stores of
 * one access inside a page. */
template <unsigned Size, bool Sign, bool Load, int Address>
unsigned single_transfer(CpuState &cpu, const FastOp &op, Memory &memory) noexcept
{
	if (!passes(cpu, op)) [[unlikely]]
		return advance(cpu, op);
	std::uint32_t address;
	std::uint32_t offset_address = 0;
	if (Address == address_constant) {
		address = op.imm;
	} else {
		const std::uint32_t base = reg(cpu, op.n);
		std::uint32_t offset;
		if (Address == address_immediate)
			offset = op.imm;
		else if (Address == address_register)
			offset = reg(cpu, op.m);
		else
			offset = shift_c(reg(cpu, op.m), static_cast<SRType>(op.shift_t), op.shift_n,
				flag_c(cpu)).value;
		offset_address = op.add ? base + offset : base - offset;
		address = op.index ? offset_address : base;
	}
	if (Size > 1U && (address & (guest_page_size - 1U)) > guest_page_size - Size) [[unlikely]]
		return fast_fallback;
	std::byte *host = memory.translate(address, Load ? MemoryAccess::read : MemoryAccess::write);
	if (host == nullptr) [[unlikely]]
		return fast_fallback;
	if (Load) {
		std::uint32_t data;
		if (Size == 1U) {
			const auto value = std::to_integer<std::uint8_t>(*host);
			data = Sign ? static_cast<std::uint32_t>(static_cast<std::int32_t>(
				static_cast<std::int8_t>(value))) : value;
		} else if (Size == 2U) {
			std::uint16_t value;
			std::memcpy(&value, host, 2);
			data = Sign ? static_cast<std::uint32_t>(static_cast<std::int32_t>(
				static_cast<std::int16_t>(value))) : value;
		} else {
			std::memcpy(&data, host, 4);
		}
		if (Address != address_constant && op.wback)
			cpu.registers[op.n] = offset_address;
		cpu.registers[op.d] = data;
	} else {
		const std::uint32_t value = reg(cpu, op.d);
		std::memcpy(host, &value, Size);
		/* What the runtime's A32Memory::stored() does after every store. */
		cpu.clear_exclusive_monitor();
		if (Address != address_constant && op.wback)
			cpu.registers[op.n] = offset_address;
	}
	return advance(cpu, op);
}

template <int Address>
[[nodiscard]] FastOp::Run single_transfer_for(Op op) noexcept
{
	switch (op) {
	case Op::ldr: return single_transfer<4, false, true, Address>;
	case Op::ldrb: return single_transfer<1, false, true, Address>;
	case Op::ldrh: return single_transfer<2, false, true, Address>;
	case Op::ldrsb: return single_transfer<1, true, true, Address>;
	case Op::ldrsh: return single_transfer<2, true, true, Address>;
	case Op::str: return single_transfer<4, false, false, Address>;
	case Op::strb: return single_transfer<1, false, false, Address>;
	case Op::strh: return single_transfer<2, false, false, Address>;
	default: return nullptr;
	}
}

/* exec_multiple_transfer() when the block of words is aligned and inside one
 * page. `imm` is the offset of the first word from the base and `imm2` the
 * change of the base. */
template <bool Load>
unsigned multiple_transfer(CpuState &cpu, const FastOp &op, Memory &memory) noexcept
{
	if (!passes(cpu, op)) [[unlikely]]
		return advance(cpu, op);
	const std::uint32_t base = reg(cpu, op.n);
	const std::uint32_t start = base + op.imm;
	const std::uint32_t span = 4U * op.shift_n;
	if ((start & 3U) != 0U || (start & (guest_page_size - 1U)) > guest_page_size - span) [[unlikely]]
		return fast_fallback;
	std::byte *host = memory.translate(start, Load ? MemoryAccess::read : MemoryAccess::write);
	if (host == nullptr) [[unlikely]]
		return fast_fallback;
	unsigned registers = op.registers;
	if (Load) {
		/* The words are read before any register changes; the page is not
		 * the register file, so reading as they are assigned is the same. */
		std::uint32_t target = 0;
		const bool to_pc = (registers & 0x8000U) != 0U;
		for (registers &= 0x7fffU; registers != 0U; registers &= registers - 1U) {
			std::uint32_t word;
			std::memcpy(&word, host, 4);
			host += 4;
			cpu.registers[std::countr_zero(registers)] = word;
		}
		if (to_pc)
			std::memcpy(&target, host, 4);
		if (op.wback)
			cpu.registers[op.n] = base + op.imm2;
		if (to_pc)
			return jump(cpu, op, key_from_target(target));
	} else {
		for (; registers != 0U; registers &= registers - 1U) {
			const std::uint32_t word = reg(cpu, static_cast<unsigned>(std::countr_zero(registers)));
			std::memcpy(host, &word, 4);
			host += 4;
		}
		cpu.clear_exclusive_monitor();
		if (op.wback)
			cpu.registers[op.n] = base + op.imm2;
	}
	return advance(cpu, op);
}

/* B, and BL: `key` is the target, `imm` the link value. */
unsigned branch(CpuState &cpu, const FastOp &op, Memory &) noexcept
{
	if (!passes(cpu, op))
		return advance(cpu, op);
	return jump(cpu, op, op.key);
}

unsigned branch_link(CpuState &cpu, const FastOp &op, Memory &) noexcept
{
	if (!passes(cpu, op))
		return advance(cpu, op);
	cpu.registers[reg_lr] = op.imm;
	return jump(cpu, op, op.key);
}

unsigned branch_exchange(CpuState &cpu, const FastOp &op, Memory &) noexcept
{
	if (!passes(cpu, op))
		return advance(cpu, op);
	return jump(cpu, op, key_from_target(reg(cpu, op.m)));
}

unsigned branch_link_exchange(CpuState &cpu, const FastOp &op, Memory &) noexcept
{
	if (!passes(cpu, op))
		return advance(cpu, op);
	const std::uint32_t target = reg(cpu, op.m);
	cpu.registers[reg_lr] = op.imm;
	return jump(cpu, op, key_from_target(target));
}

/* CBZ and CBNZ: `add` is the nonzero operand. */
unsigned compare_branch(CpuState &cpu, const FastOp &op, Memory &) noexcept
{
	if (!passes(cpu, op))
		return advance(cpu, op);
	const bool zero = reg(cpu, op.n) == 0U;
	if (op.add != zero)
		return jump(cpu, op, op.key);
	return advance(cpu, op);
}

[[nodiscard]] constexpr bool is_core(std::int32_t r) noexcept { return r >= 0 && r < 15; }

} // namespace
} // namespace detail

bool prepare_fast(const Instruction &instruction, std::uint64_t key, FastOp &out) noexcept
{
	using namespace detail;
	static constexpr auto masks = [] {
		std::array<std::uint16_t, 16> table{};
		for (unsigned cond = 0; cond < 16U; ++cond)
			table[cond] = condition_mask(cond);
		return table;
	}();
	if (instruction.status != Status::ok || instruction.form == Form::ReservedHint
		|| !key_is_aarch32(key) || instruction.cond > 15U)
		return false;
	const FormInfo &form = instruction.info();
	const OpInfo &info = op_info(instruction.form);
	if (!covered(form, info) || (form.traits & trait::fp_state) != 0U
		|| form.check != Check::none)
		return false;
	const Operands &o = instruction.operands;
	const bool thumb = key_is_thumb(key);
	const std::uint32_t pc = key_pc(key);
	const std::uint32_t pc_value = pc + (thumb ? 4U : 8U);
	const std::uint32_t next = pc + instruction.length;

	FastOp op{};
	op.next_key = thumb ? t32_key(next) : a32_key(next);
	op.next_it = instruction.next_it.bits();
	/* run_family() tests the condition of a conditional form only. */
	op.cond_mask = form.conditional ? masks[instruction.cond] : std::uint16_t{0xffffU};

	switch (info.op) {
	case Op::movw:
		if (!is_core(o.d))
			return false;
		op.d = static_cast<std::uint8_t>(o.d);
		op.imm = static_cast<std::uint32_t>(o.imm32);
		op.run = move_constant;
		break;
	case Op::movt:
		if (!is_core(o.d))
			return false;
		op.d = static_cast<std::uint8_t>(o.d);
		op.imm = std::uint32_t{o.imm16} << 16;
		op.run = move_top;
		break;
	case Op::adr: {
		if (!is_core(o.d))
			return false;
		const std::uint32_t base = pc_value & ~3U;
		const auto offset = static_cast<std::uint32_t>(o.imm32);
		op.d = static_cast<std::uint8_t>(o.d);
		op.imm = o.add ? base + offset : base - offset;
		op.run = move_constant;
		break;
	}
	case Op::adc: case Op::add: case Op::addw: case Op::and_: case Op::bic:
	case Op::cmn: case Op::cmp: case Op::eor: case Op::mov: case Op::mvn:
	case Op::orn: case Op::orr: case Op::rsb: case Op::rsc: case Op::sbc:
	case Op::sub: case Op::subw: case Op::teq: case Op::tst: {
		const bool compare = is_compare(info.op);
		const bool wants_n = info.op != Op::mov && info.op != Op::mvn;
		const std::int32_t n = info.sp_base ? static_cast<std::int32_t>(reg_sp) : o.n;
		if ((!compare && !is_core(o.d)) || (wants_n && !is_core(n)))
			return false;
		op.d = compare ? std::uint8_t{0} : static_cast<std::uint8_t>(o.d);
		op.n = wants_n ? static_cast<std::uint8_t>(n) : std::uint8_t{0};
		const bool flags = compare || o.setflags;
		if (info.shape == Shape::immediate) {
			if (is_logical(info.op) && info.op != Op::mov) {
				const auto expanded = o.a32 ? a32_expand_imm_c(o.imm)
							    : t32_expand_imm_c(o.imm);
				op.imm = static_cast<std::uint32_t>(expanded.imm32);
				op.carry = expanded.carry_in ? 0U : expanded.carry != 0U ? 2U : 1U;
			} else {
				op.imm = static_cast<std::uint32_t>(o.imm32);
				op.carry = o.carry_flags ? 0U : o.carry != 0U ? 2U : 1U;
			}
			op.run = flags ? data_processing_for<shape_immediate, true>(info.op)
				       : data_processing_for<shape_immediate, false>(info.op);
		} else if (info.shape == Shape::reg) {
			if (!is_core(o.m) || o.shift_n < 0 || o.shift_n > 32)
				return false;
			op.m = static_cast<std::uint8_t>(o.m);
			op.shift_t = static_cast<std::uint8_t>(o.shift_t);
			op.shift_n = static_cast<std::uint8_t>(o.shift_n);
			if (o.shift_n == 0 && o.shift_t != SRType::RRX)
				op.run = flags ? data_processing_for<shape_register, true>(info.op)
					       : data_processing_for<shape_register, false>(info.op);
			else
				op.run = flags ? data_processing_for<shape_shifted, true>(info.op)
					       : data_processing_for<shape_shifted, false>(info.op);
		} else {
			return false;
		}
		break;
	}
	case Op::sxtb: case Op::sxth: case Op::uxtb: case Op::uxth:
		if (!is_core(o.d) || !is_core(o.m) || o.rotation < 0 || o.rotation > 31)
			return false;
		op.d = static_cast<std::uint8_t>(o.d);
		op.m = static_cast<std::uint8_t>(o.m);
		op.shift_n = static_cast<std::uint8_t>(o.rotation);
		op.run = info.op == Op::sxtb ? extend<8, true> : info.op == Op::sxth ? extend<16, true>
			: info.op == Op::uxtb ? extend<8, false> : extend<16, false>;
		break;
	case Op::mul:
		if (!is_core(o.d) || !is_core(o.n) || !is_core(o.m))
			return false;
		op.d = static_cast<std::uint8_t>(o.d);
		op.n = static_cast<std::uint8_t>(o.n);
		op.m = static_cast<std::uint8_t>(o.m);
		op.run = o.setflags ? multiply<true> : multiply<false>;
		break;
	case Op::it: case Op::nop:
		op.run = nothing;
		break;
	case Op::ldr: case Op::ldrb: case Op::ldrh: case Op::ldrsb: case Op::ldrsh:
	case Op::str: case Op::strb: case Op::strh: {
		if (!is_core(o.t))
			return false;
		op.d = static_cast<std::uint8_t>(o.t);
		if (info.shape == Shape::literal) {
			const std::uint32_t base = pc_value & ~3U;
			const auto offset = static_cast<std::uint32_t>(o.imm32);
			op.imm = o.add ? base + offset : base - offset;
			op.run = single_transfer_for<address_constant>(info.op);
			break;
		}
		if (!is_core(o.n))
			return false;
		op.n = static_cast<std::uint8_t>(o.n);
		op.add = o.add;
		op.index = o.index;
		op.wback = o.wback;
		if (info.shape == Shape::reg) {
			if (!is_core(o.m) || o.shift_n < 0 || o.shift_n > 32)
				return false;
			op.m = static_cast<std::uint8_t>(o.m);
			op.shift_t = static_cast<std::uint8_t>(o.shift_t);
			op.shift_n = static_cast<std::uint8_t>(o.shift_n);
			op.run = o.shift_n == 0 && o.shift_t != SRType::RRX
				? single_transfer_for<address_register>(info.op)
				: single_transfer_for<address_shifted>(info.op);
		} else {
			op.imm = static_cast<std::uint32_t>(o.imm32);
			op.run = single_transfer_for<address_immediate>(info.op);
		}
		break;
	}
	case Op::ldm: case Op::stm: {
		const bool load = info.op == Op::ldm;
		const std::int32_t n = info.stack ? static_cast<std::int32_t>(reg_sp) : o.n;
		const unsigned registers = o.registers;
		const unsigned count = static_cast<unsigned>(std::popcount(registers));
		/* A stored PC reads PC32(); leave that form to the library. */
		if (!is_core(n) || count == 0U || (!load && (registers & 0x8000U) != 0U))
			return false;
		const std::uint32_t span = 4U * count;
		switch (info.transfer) {
		case Mode::ia: op.imm = 0U; op.imm2 = span; break;
		case Mode::ib: op.imm = 4U; op.imm2 = span; break;
		case Mode::da: op.imm = 4U - span; op.imm2 = 0U - span; break;
		default: op.imm = 0U - span; op.imm2 = 0U - span; break;
		}
		op.n = static_cast<std::uint8_t>(n);
		op.registers = static_cast<std::uint16_t>(registers);
		op.shift_n = static_cast<std::uint8_t>(count);
		op.wback = info.stack ? true : o.wback;
		op.run = load ? multiple_transfer<true> : multiple_transfer<false>;
		break;
	}
	case Op::b: {
		const std::uint32_t target = pc_value + static_cast<std::uint32_t>(o.imm32);
		op.key = thumb ? t32_key(target & ~1U) : a32_key(target & ~3U);
		op.run = branch;
		break;
	}
	case Op::bl: {
		op.imm = thumb ? pc_value | 1U : pc_value - 4U;
		const bool to_thumb = o.targetInstrSet == InstrSet::T32;
		const std::uint32_t base = o.targetInstrSet == InstrSet::A32 ? pc_value & ~3U : pc_value;
		const std::uint32_t target = base + static_cast<std::uint32_t>(o.imm32);
		op.key = to_thumb ? t32_key(target & ~1U) : a32_key(target & ~3U);
		op.run = branch_link;
		break;
	}
	case Op::blx:
		if (!is_core(o.m))
			return false;
		op.m = static_cast<std::uint8_t>(o.m);
		op.imm = thumb ? (pc + 2U) | 1U : pc + 4U;
		op.run = branch_link_exchange;
		break;
	case Op::bx:
		if (!is_core(o.m))
			return false;
		op.m = static_cast<std::uint8_t>(o.m);
		op.run = branch_exchange;
		break;
	case Op::cbz: case Op::cbnz: {
		if (!is_core(o.n))
			return false;
		const std::uint32_t target = pc_value + static_cast<std::uint32_t>(o.imm32);
		op.n = static_cast<std::uint8_t>(o.n);
		op.add = o.nonzero;
		op.key = thumb ? t32_key(target & ~1U) : a32_key(target & ~3U);
		op.run = compare_branch;
		break;
	}
	default:
		return false;
	}
	if (op.run == nullptr)
		return false;
	out = op;
	return true;
}

} // namespace uml::cpu::arm64::aarch32
