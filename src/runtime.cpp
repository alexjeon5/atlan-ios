// SPDX-License-Identifier: GPL-2.0-or-later
#include "atlan/runtime.hpp"
#include "atlan/vfp.hpp"
#include "cpu/arm64/aarch32/guest_key.hpp"
#include "cpu/arm64/aarch32/semantics.hpp"
#include <bit>
#include <cstring>
#include <stdexcept>

namespace atlan {
namespace a32 = uml::cpu::arm64::aarch32;
static_assert(std::endian::native == std::endian::little, "little-endian host required");
namespace {
class MemoryAdapter final : public a32::A32Memory {
public:
    MemoryAdapter(Memory& memory, CpuState& cpu) : memory_(memory), cpu_(cpu) {}
    std::byte* translate(std::uint64_t address, MemoryAccess access) noexcept override {
        return memory_.translate(address, access);
    }
    void stored(std::byte*, std::size_t) noexcept override {
        // Conservative single-vCPU reservation invalidation after every store.
        cpu_.clear_exclusive_monitor();
    }
private:
    Memory& memory_;
    CpuState& cpu_;
};
}
void Runtime::start(std::uint32_t target) noexcept {
    cpu.pc = a32::key_from_target(target);
    cpu.a32_itstate = 0;
    cpu.clear_exclusive_monitor();
}
std::uint32_t Runtime::pc() const noexcept { return a32::key_pc(cpu.pc); }
bool Runtime::thumb() const noexcept { return a32::key_is_thumb(cpu.pc); }
Result Runtime::step() noexcept {
    if (!a32::key_is_thumb(cpu.pc) && a32::key_is_canonical(cpu.pc)) {
        // The integer core does not execute VFP; route those A32 words first.
        std::uint32_t word{};
        const std::uint32_t at = a32::key_pc(cpu.pc);
        if (const auto* host = memory.translate(at, MemoryAccess::execute)) {
            std::memcpy(&word, host, 4);
            const auto done = vfp::execute_a32(cpu, memory, word, at);
            switch (done.kind) {
            case vfp::Execution::Kind::not_vfp: break;
            case vfp::Execution::Kind::retired: return {Event::retired, 1};
            case vfp::Execution::Kind::fault:
                return {Event::memory_fault, 0, done.address, 0, done.access, done.alignment};
            case vfp::Execution::Kind::unsupported:
                return {Event::unsupported_instruction, 0, at, word, MemoryAccess::execute, false, done.reason};
            }
        }
    }
    MemoryAdapter adapter(memory, cpu);
    const auto result = a32::step_a32(cpu, adapter, a32::Profile{a32::FeatureSet{}});
    Event event;
    switch (result.event) {
    case a32::StepEvent::retired: event = Event::retired; break;
    case a32::StepEvent::supervisor_call: event = Event::supervisor_call; break;
    case a32::StepEvent::breakpoint: event = Event::breakpoint; break;
    case a32::StepEvent::undefined: event = Event::undefined_instruction; break;
    case a32::StepEvent::memory_fault: event = Event::memory_fault; break;
    case a32::StepEvent::pc_alignment: event = Event::pc_alignment; break;
    case a32::StepEvent::fatal: event = Event::unsupported_instruction; break;
    default: event = Event::unsupported_instruction; break;
    }
    return {event, event == Event::retired || event == Event::supervisor_call ? 1U : 0U,
            result.fault_address, result.syndrome, result.access, result.alignment};
}
Result Runtime::run(std::uint64_t budget) noexcept {
    std::uint64_t retired = 0;
    while (retired < budget) {
        auto result = step();
        retired += result.retired;
        if (result.event != Event::retired) {
            result.retired = retired;
            return result;
        }
    }
    return {Event::budget_exhausted, retired};
}
Result Runtime::call(std::uint32_t target, std::span<const std::uint32_t> arguments,
                     std::uint32_t stack_top, std::uint32_t return_target,
                     std::uint64_t budget) {
    if (arguments.size() > 4 || stack_top % 8 != 0 || return_target % 4 != 0 ||
        (target & ~1U) == return_target ||
        memory.translate(return_target, MemoryAccess::read) != nullptr)
        throw std::invalid_argument("invalid scalar AAPCS32 call frame");
    for (std::size_t i = 0; i < 4; ++i)
        cpu.registers[i] = i < arguments.size() ? arguments[i] : 0;
    cpu.registers[13] = stack_top;
    cpu.registers[14] = return_target;
    start(target);
    std::uint64_t retired = 0;
    while (true) {
        if (!thumb() && pc() == return_target) return {Event::function_return, retired};
        if (retired == budget) return {Event::budget_exhausted, retired};
        auto result = step();
        retired += result.retired;
        if (result.event != Event::retired) {
            result.retired = retired;
            return result;
        }
    }
}
const char* event_name(Event event) noexcept {
    switch (event) {
    case Event::retired: return "retired";
    case Event::supervisor_call: return "supervisor_call";
    case Event::breakpoint: return "breakpoint";
    case Event::undefined_instruction: return "undefined_instruction";
    case Event::memory_fault: return "memory_fault";
    case Event::pc_alignment: return "pc_alignment";
    case Event::unsupported_instruction: return "unsupported_instruction";
    case Event::budget_exhausted: return "budget_exhausted";
    case Event::function_return: return "function_return";
    }
    return "unknown";
}
}
