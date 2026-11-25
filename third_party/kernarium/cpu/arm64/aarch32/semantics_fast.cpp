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

/* clear_exclusive_monitor() after a store. A held reservation always has a
 * 32-bit address, so an address of ~0 means every field is already clear. */
inline void stored(CpuState &cpu) noexcept
{
	if (cpu.exclusive_address != ~std::uint64_t{0}) [[unlikely]]
		cpu.clear_exclusive_monitor();
}

/* The caller writes back next_key and next_it (see fast_next). */
[[nodiscard]] inline unsigned advance(CpuState &, const FastOp &) noexcept
{
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
 * an immediate, a register shifted by a register (`shift_n` names it), and a
 * register shifted left by 1-31 where the shifter carry is not used. */
enum : int { shape_immediate, shape_register, shape_shifted, shape_by_register, shape_lsl };

/* exec_data_processing() for one operation, shape and flag setting. With
 * FirstConstant the first operand is the PC, whose value is `imm2`; with ToPc
 * the destination is the PC (ALUWritePC, `index` set in T32). */
template <Op Operation, int Shape, bool SetFlags, bool FirstConstant, bool ToPc>
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
	} else if (Shape == shape_lsl) {
		b = reg(cpu, op.m) << op.shift_n;
		shifter_carry = false;  /* not read: see prepare_fast() */
	} else {
		const unsigned amount = Shape == shape_by_register ? reg(cpu, op.shift_n) & 0xffU
								   : op.shift_n;
		const auto shifted = shift_c(reg(cpu, op.m), static_cast<SRType>(op.shift_t),
			amount, flag_c(cpu));
		b = shifted.value;
		shifter_carry = shifted.carry;
	}
	const std::uint32_t a = Operation == Op::mov || Operation == Op::mvn ? 0U
		: FirstConstant ? op.imm2 : reg(cpu, op.n);
	std::uint32_t result;
	bool carry = shifter_carry;
	/* AddWithCarry() with C and V as flag bits 29 and 28, computed without
	 * the 64-bit signed sum. */
	std::uint32_t cv = 0;
	const auto add = [&](std::uint32_t first, std::uint32_t second, bool carry_in) {
		const std::uint64_t sum = std::uint64_t{first} + second + (carry_in ? 1U : 0U);
		result = static_cast<std::uint32_t>(sum);
		cv = static_cast<std::uint32_t>(sum >> 32) << 29
			| ((~(first ^ second) & (first ^ result)) >> 31) << 28;
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
	if (ToPc && !is_compare(Operation))
		return jump(cpu, op, op.index ? t32_key(result & ~1U) : key_from_target(result));
	if (!is_compare(Operation))
		cpu.registers[op.d] = result;
	if (SetFlags) {
		if (is_logical(Operation))
			set_nzc(cpu, result, carry);
		else
			cpu.nzcv = (cpu.nzcv & 0x0fff'ffffU) | (result & 0x8000'0000U)
				| (result == 0U ? 0x4000'0000U : 0U) | cv;
	}
	return advance(cpu, op);
}

template <int Shape, bool SetFlags, bool FirstConstant, bool ToPc>
[[nodiscard]] FastOp::Run data_processing_as(Op op) noexcept
{
	switch (op) {
#define UML_A32_FAST_DP(name) \
	case Op::name: return data_processing<Op::name, Shape, SetFlags, FirstConstant, ToPc>;
	UML_A32_FAST_DP(add) UML_A32_FAST_DP(addw) UML_A32_FAST_DP(cmn) UML_A32_FAST_DP(adc)
	UML_A32_FAST_DP(sub) UML_A32_FAST_DP(subw) UML_A32_FAST_DP(cmp) UML_A32_FAST_DP(sbc)
	UML_A32_FAST_DP(rsb) UML_A32_FAST_DP(rsc) UML_A32_FAST_DP(and_) UML_A32_FAST_DP(tst)
	UML_A32_FAST_DP(eor) UML_A32_FAST_DP(teq) UML_A32_FAST_DP(orr) UML_A32_FAST_DP(orn)
	UML_A32_FAST_DP(bic) UML_A32_FAST_DP(mov) UML_A32_FAST_DP(mvn)
#undef UML_A32_FAST_DP
	default: return nullptr;
	}
}

template <int Shape>
[[nodiscard]] FastOp::Run data_processing_for(Op op, bool flags, bool first_constant,
	bool to_pc) noexcept
{
	if (to_pc)
		return first_constant ? data_processing_as<Shape, false, true, true>(op)
				      : data_processing_as<Shape, false, false, true>(op);
	if (flags)
		return first_constant ? data_processing_as<Shape, true, true, false>(op)
				      : data_processing_as<Shape, true, false, false>(op);
	return first_constant ? data_processing_as<Shape, false, true, false>(op)
			      : data_processing_as<Shape, false, false, false>(op);
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

/* MLA and MLS: `shift_n` names the addend. */
template <bool Subtract, bool SetFlags>
unsigned multiply_accumulate(CpuState &cpu, const FastOp &op, Memory &) noexcept
{
	if (passes(cpu, op)) [[likely]] {
		const std::uint32_t product = reg(cpu, op.n) * reg(cpu, op.m);
		const std::uint32_t result = Subtract ? reg(cpu, op.shift_n) - product
						      : product + reg(cpu, op.shift_n);
		cpu.registers[op.d] = result;
		if (SetFlags)
			set_nzc(cpu, result, flag_c(cpu));
	}
	return advance(cpu, op);
}

/* UMULL, SMULL, UMLAL, SMLAL: `d` is RdLo and `shift_t` RdHi (written in that
 * order, as exec_multiply() does). */
template <bool Signed, bool Accumulate, bool SetFlags>
unsigned multiply_long(CpuState &cpu, const FastOp &op, Memory &) noexcept
{
	if (passes(cpu, op)) [[likely]] {
		std::uint64_t result = Signed
			? static_cast<std::uint64_t>(std::int64_t{static_cast<std::int32_t>(reg(cpu, op.n))}
				* std::int64_t{static_cast<std::int32_t>(reg(cpu, op.m))})
			: std::uint64_t{reg(cpu, op.n)} * std::uint64_t{reg(cpu, op.m)};
		if (Accumulate)
			result += std::uint64_t{reg(cpu, op.shift_t)} << 32 | reg(cpu, op.d);
		cpu.registers[op.d] = static_cast<std::uint32_t>(result);
		cpu.registers[op.shift_t] = static_cast<std::uint32_t>(result >> 32);
		if (SetFlags)
			cpu.nzcv = (cpu.nzcv & 0x3fff'ffffU)
				| static_cast<std::uint32_t>(result >> 32 & 0x8000'0000U)
				| (result == 0U ? 0x4000'0000U : 0U);
	}
	return advance(cpu, op);
}

/* SMLA<x><y> (Accumulate) and SMUL<x><y>: `add` and `index` pick the top
 * halves of Rn and Rm, `shift_n` names the addend; overflow of the addition
 * sets Q. */
template <bool NTop, bool MTop, bool Accumulate>
unsigned multiply_halves(CpuState &cpu, const FastOp &op, Memory &) noexcept
{
	if (passes(cpu, op)) [[likely]] {
		const std::uint32_t n = reg(cpu, op.n), m = reg(cpu, op.m);
		const std::int32_t a = NTop ? static_cast<std::int32_t>(n) >> 16
			: static_cast<std::int16_t>(n);
		const std::int32_t b = MTop ? static_cast<std::int32_t>(m) >> 16
			: static_cast<std::int16_t>(m);
		const std::int64_t product = std::int64_t{a} * b;
		if (!Accumulate) {
			cpu.registers[op.d] = static_cast<std::uint32_t>(product);
		} else {
			const std::int64_t result = product
				+ std::int64_t{static_cast<std::int32_t>(reg(cpu, op.shift_n))};
			cpu.registers[op.d] = static_cast<std::uint32_t>(result);
			if (result != std::int64_t{static_cast<std::int32_t>(result)})
				cpu.a32_q = 1;
		}
	}
	return advance(cpu, op);
}

template <bool Accumulate>
[[nodiscard]] FastOp::Run multiply_halves_for(bool n_top, bool m_top) noexcept
{
	return n_top ? (m_top ? multiply_halves<true, true, Accumulate> : multiply_halves<true, false, Accumulate>)
		     : (m_top ? multiply_halves<false, true, Accumulate> : multiply_halves<false, false, Accumulate>);
}

/* How a single transfer forms its address: a constant (the literal pool), the
 * base and an immediate, the base and a register, the base and a shifted
 * register. */
enum : int { address_constant, address_immediate, address_register, address_shifted, address_lsl };

/* exec_single_transfer() for the byte, halfword and word loads and stores of
 * one access inside a page. With BaseConstant the base is the PC, whose value
 * is `imm2`; with ToPc a word load writes the PC (LoadWritePC). */
template <unsigned Size, bool Sign, bool Load, int Address, bool BaseConstant, bool ToPc>
unsigned single_transfer(CpuState &cpu, const FastOp &op, Memory &memory) noexcept
{
	if (!passes(cpu, op)) [[unlikely]]
		return advance(cpu, op);
	std::uint32_t address;
	std::uint32_t offset_address = 0;
	if (Address == address_constant) {
		address = op.imm;
	} else {
		const std::uint32_t base = BaseConstant ? op.imm2 : reg(cpu, op.n);
		std::uint32_t offset;
		if (Address == address_immediate)
			offset = op.imm;
		else if (Address == address_register)
			offset = reg(cpu, op.m);
		else if (Address == address_lsl)
			offset = reg(cpu, op.m) << op.shift_n;
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
		if (Address != address_constant && !BaseConstant && op.wback)
			cpu.registers[op.n] = offset_address;
		if (ToPc)
			return jump(cpu, op, key_from_target(data));
		cpu.registers[op.d] = data;
	} else {
		const std::uint32_t value = reg(cpu, op.d);
		std::memcpy(host, &value, Size);
		/* What the runtime's A32Memory::stored() does after every store. */
		stored(cpu);
		if (Address != address_constant && !BaseConstant && op.wback)
			cpu.registers[op.n] = offset_address;
	}
	return advance(cpu, op);
}

template <int Address, bool BaseConstant>
[[nodiscard]] FastOp::Run single_transfer_as(Op op, bool to_pc) noexcept
{
	if (to_pc)
		return op == Op::ldr ? single_transfer<4, false, true, Address, BaseConstant, true>
				     : nullptr;
	switch (op) {
	case Op::ldr: return single_transfer<4, false, true, Address, BaseConstant, false>;
	case Op::ldrb: return single_transfer<1, false, true, Address, BaseConstant, false>;
	case Op::ldrh: return single_transfer<2, false, true, Address, BaseConstant, false>;
	case Op::ldrsb: return single_transfer<1, true, true, Address, BaseConstant, false>;
	case Op::ldrsh: return single_transfer<2, true, true, Address, BaseConstant, false>;
	case Op::str: return single_transfer<4, false, false, Address, BaseConstant, false>;
	case Op::strb: return single_transfer<1, false, false, Address, BaseConstant, false>;
	case Op::strh: return single_transfer<2, false, false, Address, BaseConstant, false>;
	default: return nullptr;
	}
}

template <int Address>
[[nodiscard]] FastOp::Run single_transfer_for(Op op, bool base_constant, bool to_pc) noexcept
{
	return base_constant ? single_transfer_as<Address, true>(op, to_pc)
			     : single_transfer_as<Address, false>(op, to_pc);
}

/* LDRD and STRD with an immediate or register offset: `d` is Rt and
 * `shift_t` Rt2. A doubleword access must be word aligned; an unaligned one
 * or one that crosses a page falls back. */
template <bool Load, int Address>
unsigned doubleword_transfer(CpuState &cpu, const FastOp &op, Memory &memory) noexcept
{
	if (!passes(cpu, op)) [[unlikely]]
		return advance(cpu, op);
	const std::uint32_t base = reg(cpu, op.n);
	const std::uint32_t offset = Address == address_immediate ? op.imm : reg(cpu, op.m);
	const std::uint32_t offset_address = op.add ? base + offset : base - offset;
	const std::uint32_t address = op.index ? offset_address : base;
	if ((address & 3U) != 0U || (address & (guest_page_size - 1U)) > guest_page_size - 8U) [[unlikely]]
		return fast_fallback;
	std::byte *host = memory.translate(address, Load ? MemoryAccess::read : MemoryAccess::write);
	if (host == nullptr) [[unlikely]]
		return fast_fallback;
	if (Load) {
		std::uint32_t low, high;
		std::memcpy(&low, host, 4);
		std::memcpy(&high, host + 4, 4);
		if (op.wback)
			cpu.registers[op.n] = offset_address;
		cpu.registers[op.d] = low;
		cpu.registers[op.shift_t] = high;
	} else {
		const std::uint32_t low = reg(cpu, op.d), high = reg(cpu, op.shift_t);
		std::memcpy(host, &low, 4);
		std::memcpy(host + 4, &high, 4);
		stored(cpu);
		if (op.wback)
			cpu.registers[op.n] = offset_address;
	}
	return advance(cpu, op);
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
		stored(cpu);
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
		const bool flags = compare || o.setflags;
		const bool to_pc = !compare && o.d == static_cast<std::int32_t>(reg_pc);
		const bool first_constant = wants_n && n == static_cast<std::int32_t>(reg_pc);
		if ((!compare && !to_pc && !is_core(o.d)) || (to_pc && flags)
			|| (wants_n && !first_constant && !is_core(n)))
			return false;
		op.d = compare || to_pc ? std::uint8_t{0} : static_cast<std::uint8_t>(o.d);
		op.n = wants_n && !first_constant ? static_cast<std::uint8_t>(n) : std::uint8_t{0};
		op.imm2 = pc_value;
		op.index = thumb;
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
			op.run = data_processing_for<shape_immediate>(info.op, flags, first_constant, to_pc);
		} else if (info.shape == Shape::reg) {
			if (o.shift_n < 0 || o.shift_n > 32)
				return false;
			if (o.m == static_cast<std::int32_t>(reg_pc)) {
				/* The PC as the second operand is a constant; its shifter
				 * carry would need the flags, so only without them. */
				if (flags || o.shift_t == SRType::RRX)
					return false;
				op.imm = shift_c(pc_value, o.shift_t, static_cast<unsigned>(o.shift_n), false).value;
				op.run = data_processing_for<shape_immediate>(info.op, false, first_constant, to_pc);
				break;
			}
			if (!is_core(o.m))
				return false;
			op.m = static_cast<std::uint8_t>(o.m);
			op.shift_t = static_cast<std::uint8_t>(o.shift_t);
			op.shift_n = static_cast<std::uint8_t>(o.shift_n);
			if (o.shift_n == 0 && o.shift_t != SRType::RRX)
				op.run = data_processing_for<shape_register>(info.op, flags, first_constant, to_pc);
			else if (o.shift_t == SRType::LSL && o.shift_n < 32 && !(flags && is_logical(info.op)))
				op.run = data_processing_for<shape_lsl>(info.op, flags, first_constant, to_pc);
			else
				op.run = data_processing_for<shape_shifted>(info.op, flags, first_constant, to_pc);
		} else if (info.shape == Shape::reg_shifted) {
			if (!is_core(o.m) || !is_core(o.s))
				return false;
			op.m = static_cast<std::uint8_t>(o.m);
			op.shift_t = static_cast<std::uint8_t>(o.shift_t);
			op.shift_n = static_cast<std::uint8_t>(o.s);
			op.run = data_processing_for<shape_by_register>(info.op, flags, first_constant, to_pc);
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
	case Op::mla: case Op::mls:
		if (!is_core(o.d) || !is_core(o.n) || !is_core(o.m) || !is_core(o.a))
			return false;
		op.d = static_cast<std::uint8_t>(o.d);
		op.n = static_cast<std::uint8_t>(o.n);
		op.m = static_cast<std::uint8_t>(o.m);
		op.shift_n = static_cast<std::uint8_t>(o.a);
		op.run = info.op == Op::mls
			? (o.setflags ? multiply_accumulate<true, true> : multiply_accumulate<true, false>)
			: (o.setflags ? multiply_accumulate<false, true> : multiply_accumulate<false, false>);
		break;
	case Op::umull: case Op::smull: case Op::umlal: case Op::smlal: {
		if (!is_core(o.dLo) || !is_core(o.dHi) || !is_core(o.n) || !is_core(o.m))
			return false;
		op.d = static_cast<std::uint8_t>(o.dLo);
		op.shift_t = static_cast<std::uint8_t>(o.dHi);
		op.n = static_cast<std::uint8_t>(o.n);
		op.m = static_cast<std::uint8_t>(o.m);
		const bool is_signed = info.op == Op::smull || info.op == Op::smlal;
		const bool accumulate = info.op == Op::umlal || info.op == Op::smlal;
		if (is_signed)
			op.run = accumulate ? (o.setflags ? multiply_long<true, true, true> : multiply_long<true, true, false>)
					    : (o.setflags ? multiply_long<true, false, true> : multiply_long<true, false, false>);
		else
			op.run = accumulate ? (o.setflags ? multiply_long<false, true, true> : multiply_long<false, true, false>)
					    : (o.setflags ? multiply_long<false, false, true> : multiply_long<false, false, false>);
		break;
	}
	case Op::smla_xy: case Op::smul_xy:
		if (!is_core(o.d) || !is_core(o.n) || !is_core(o.m)
			|| (info.op == Op::smla_xy && !is_core(o.a)))
			return false;
		op.d = static_cast<std::uint8_t>(o.d);
		op.n = static_cast<std::uint8_t>(o.n);
		op.m = static_cast<std::uint8_t>(o.m);
		op.shift_n = info.op == Op::smla_xy ? static_cast<std::uint8_t>(o.a) : std::uint8_t{0};
		op.run = info.op == Op::smla_xy ? multiply_halves_for<true>(o.n_index != 0, o.m_index != 0)
						: multiply_halves_for<false>(o.n_index != 0, o.m_index != 0);
		break;
	case Op::ldrd: case Op::strd:
		if (info.shape == Shape::literal || !is_core(o.n) || !is_core(o.t) || !is_core(o.t2))
			return false;
		op.d = static_cast<std::uint8_t>(o.t);
		op.shift_t = static_cast<std::uint8_t>(o.t2);
		op.n = static_cast<std::uint8_t>(o.n);
		op.add = o.add;
		op.index = o.index;
		op.wback = o.wback;
		if (info.shape == Shape::reg) {
			if (!is_core(o.m) || o.shift_n != 0)
				return false;
			op.m = static_cast<std::uint8_t>(o.m);
			op.run = info.op == Op::ldrd ? doubleword_transfer<true, address_register>
						     : doubleword_transfer<false, address_register>;
		} else {
			op.imm = static_cast<std::uint32_t>(o.imm32);
			op.run = info.op == Op::ldrd ? doubleword_transfer<true, address_immediate>
						     : doubleword_transfer<false, address_immediate>;
		}
		break;
	case Op::it: case Op::nop:
		op.run = nothing;
		break;
	case Op::ldr: case Op::ldrb: case Op::ldrh: case Op::ldrsb: case Op::ldrsh:
	case Op::str: case Op::strb: case Op::strh: {
		const bool to_pc = o.t == static_cast<std::int32_t>(reg_pc);
		if (!to_pc && !is_core(o.t))
			return false;
		op.d = to_pc ? std::uint8_t{0} : static_cast<std::uint8_t>(o.t);
		if (info.shape == Shape::literal) {
			const std::uint32_t base = pc_value & ~3U;
			const auto offset = static_cast<std::uint32_t>(o.imm32);
			op.imm = o.add ? base + offset : base - offset;
			op.run = single_transfer_for<address_constant>(info.op, false, to_pc);
			break;
		}
		const bool base_constant = o.n == static_cast<std::int32_t>(reg_pc);
		if ((!base_constant && !is_core(o.n)) || (base_constant && o.wback))
			return false;
		op.n = base_constant ? std::uint8_t{0} : static_cast<std::uint8_t>(o.n);
		op.imm2 = pc_value;
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
				? single_transfer_for<address_register>(info.op, base_constant, to_pc)
				: o.shift_t == SRType::LSL && o.shift_n < 32
				? single_transfer_for<address_lsl>(info.op, base_constant, to_pc)
				: single_transfer_for<address_shifted>(info.op, base_constant, to_pc);
		} else {
			op.imm = static_cast<std::uint32_t>(o.imm32);
			op.run = single_transfer_for<address_immediate>(info.op, base_constant, to_pc);
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
		op.imm = thumb ? (pc + 2U) | 1U : pc + 4U;
		if (o.m == static_cast<std::int32_t>(reg_pc)) {
			op.key = key_from_target(pc_value);
			op.run = branch_link;
			break;
		}
		if (!is_core(o.m))
			return false;
		op.m = static_cast<std::uint8_t>(o.m);
		op.run = branch_link_exchange;
		break;
	case Op::bx:
		/* BX PC: the Thumb-to-ARM veneer. */
		if (o.m == static_cast<std::int32_t>(reg_pc)) {
			op.key = key_from_target(pc_value);
			op.run = branch;
			break;
		}
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
