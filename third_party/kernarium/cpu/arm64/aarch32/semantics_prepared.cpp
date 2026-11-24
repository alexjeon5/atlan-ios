// SPDX-License-Identifier: GPL-2.0-or-later
//
// Ahead-of-time dispatch for pre-decoded instructions (semantics.hpp,
// prepare_a32). Added for the block cache; it is not part of the imported
// Kernarium sources. The operation-to-family mapping below mirrors dispatch()
// in semantics.cpp: change both together. tests/block_cache_tests.cpp compares
// the two paths over random instruction words to catch drift.

#include "cpu/arm64/aarch32/semantics_internal.hpp"

namespace uml::cpu::arm64::aarch32 {
namespace detail {
namespace {

using Family = bool (*)(Exec &, const OpInfo &);

bool exec_nothing(Exec &, const OpInfo &) { return true; }
bool exec_vector_handler(Exec &x, const OpInfo &info) { return info.vector(x); }

[[nodiscard]] Family family_of(Op op) noexcept
{
	switch (op) {
	case Op::none:
	case Op::library:
	case Op::vldr: case Op::vstr: case Op::vldm: case Op::vstm: case Op::vld:
	case Op::vst:
		return nullptr;
	case Op::adc: case Op::add: case Op::addw: case Op::adr: case Op::and_:
	case Op::bic: case Op::cmn: case Op::cmp: case Op::eor: case Op::mov:
	case Op::movw: case Op::movt: case Op::mvn: case Op::orn: case Op::orr:
	case Op::rsb: case Op::rsc: case Op::sbc: case Op::sub: case Op::subw:
	case Op::teq: case Op::tst:
		return exec_data_processing;
	case Op::bfc: case Op::bfi: case Op::sbfx: case Op::ubfx: case Op::clz:
	case Op::rbit: case Op::rev: case Op::rev16: case Op::revsh:
	case Op::sxtb: case Op::sxth: case Op::uxtb: case Op::uxth:
	case Op::sxtab: case Op::sxtah: case Op::uxtab: case Op::uxtah:
	case Op::sxtb16: case Op::uxtb16: case Op::sxtab16: case Op::uxtab16:
	case Op::pkh: case Op::sel:
		return exec_bits_and_extend;
	case Op::mul: case Op::mla: case Op::mls: case Op::umull: case Op::umlal:
	case Op::smull: case Op::smlal: case Op::umaal: case Op::sdiv:
	case Op::udiv: case Op::smla_xy: case Op::smul_xy: case Op::smlal_xy:
	case Op::smlaw: case Op::smulw: case Op::smlad: case Op::smlsd:
	case Op::smuad: case Op::smusd: case Op::smlald: case Op::smlsld:
	case Op::smmla: case Op::smmls: case Op::smmul: case Op::usad8:
	case Op::usada8:
		return exec_multiply;
	case Op::qadd: case Op::qsub: case Op::qdadd: case Op::qdsub:
	case Op::ssat: case Op::usat: case Op::ssat16: case Op::usat16:
	case Op::parallel:
		return exec_saturate_and_parallel;
	case Op::b: case Op::bl: case Op::blx: case Op::bx: case Op::cbz:
	case Op::cbnz: case Op::tb:
		return exec_branch;
	case Op::ldr: case Op::ldrb: case Op::ldrh: case Op::ldrsb:
	case Op::ldrsh: case Op::ldrd: case Op::str: case Op::strb:
	case Op::strh: case Op::strd: case Op::ldrt: case Op::ldrbt:
	case Op::ldrht: case Op::ldrsbt: case Op::ldrsht: case Op::strt:
	case Op::strbt: case Op::strht: case Op::prefetch:
		return exec_single_transfer;
	case Op::ldm: case Op::stm:
		return exec_multiple_transfer;
	case Op::ldrex: case Op::ldrexb: case Op::ldrexh: case Op::ldrexd:
	case Op::strex: case Op::strexb: case Op::strexh: case Op::strexd:
	case Op::ldaex: case Op::ldaexb: case Op::ldaexh: case Op::ldaexd:
	case Op::stlex: case Op::stlexb: case Op::stlexh: case Op::stlexd:
	case Op::lda: case Op::ldab: case Op::ldah: case Op::stl: case Op::stlb:
	case Op::stlh: case Op::clrex:
		return exec_exclusive;
	case Op::svc: case Op::bkpt: case Op::udf: case Op::mrs: case Op::msr:
	case Op::coprocessor: case Op::nop: case Op::dmb: case Op::dsb:
	case Op::isb: case Op::it:
		return exec_system;
	case Op::vector:
		return exec_vector_handler;
	}
	return nullptr;
}

/* One instantiation per family and per conditional/unconditional form, so the
 * handler address is the only dispatch left at run time. */
template <Family Fn, bool Conditional>
StepResult run_family(CpuState &cpu, const Instruction &instruction,
	A32Memory &memory, const void *info) noexcept
{
	Exec x(cpu, instruction, memory);
	if (!(Conditional && !x.condition_passed())
		&& !Fn(x, *static_cast<const OpInfo *>(info)))
		return x.result;
	if (!x.pc_written)
		cpu.pc = x.thumb ? t32_key(x.next_pc()) : a32_key(x.next_pc());
	cpu.a32_itstate = instruction.next_it.bits();
	return x.result;
}

template <Family Fn>
[[nodiscard]] PreparedInstruction::Run pick(bool conditional) noexcept
{
	return conditional ? run_family<Fn, true> : run_family<Fn, false>;
}

[[nodiscard]] PreparedInstruction::Run run_for(Family family, bool conditional) noexcept
{
	if (family == exec_data_processing) return pick<exec_data_processing>(conditional);
	if (family == exec_bits_and_extend) return pick<exec_bits_and_extend>(conditional);
	if (family == exec_multiply) return pick<exec_multiply>(conditional);
	if (family == exec_saturate_and_parallel) return pick<exec_saturate_and_parallel>(conditional);
	if (family == exec_branch) return pick<exec_branch>(conditional);
	if (family == exec_single_transfer) return pick<exec_single_transfer>(conditional);
	if (family == exec_multiple_transfer) return pick<exec_multiple_transfer>(conditional);
	if (family == exec_exclusive) return pick<exec_exclusive>(conditional);
	if (family == exec_system) return pick<exec_system>(conditional);
	if (family == exec_vector_handler) return pick<exec_vector_handler>(conditional);
	return nullptr;
}

} // namespace
} // namespace detail

PreparedInstruction prepare_a32(const Instruction &instruction) noexcept
{
	using namespace detail;
	if (instruction.status == Status::undefined)
		return {};
	/* A reserved hint and a decoded no-op retire without touching state. */
	if (instruction.form == Form::ReservedHint || instruction.status == Status::nop)
		return {run_family<exec_nothing, false>, nullptr};
	const FormInfo &form = instruction.info();
	const OpInfo &info = op_info(instruction.form);
	if (!covered(form, info))
		return {};
	const Family family = family_of(info.op);
	if (family == nullptr)
		return {};
	const auto run = run_for(family, form.conditional);
	if (run == nullptr)
		return {};
	return {run, &info};
}

} // namespace uml::cpu::arm64::aarch32
