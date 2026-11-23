// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include "atlan/cpu_state.hpp"
#include "atlan/memory.hpp"

namespace atlan {
enum class Event : std::uint8_t {
    retired, supervisor_call, breakpoint, undefined_instruction,
    memory_fault, pc_alignment, unsupported_instruction, budget_exhausted, function_return
};
struct Result {
    Event event{Event::retired};
    std::uint64_t retired{};
    std::uint32_t address{};
    std::uint32_t syndrome{};
    MemoryAccess access{MemoryAccess::read};
    bool alignment{};
    const char* detail{};  // static reason for unsupported_instruction, when known
};

// One CPU and memory instance belong to one host thread. SVC returns to the
// embedding application; it never executes a host syscall.
class Runtime {
public:
    CpuState cpu;
    Memory memory;
    void start(std::uint32_t target) noexcept;
    [[nodiscard]] std::uint32_t pc() const noexcept;
    [[nodiscard]] bool thumb() const noexcept;
    [[nodiscard]] Result step() noexcept;
    [[nodiscard]] Result run(std::uint64_t budget) noexcept;
    static constexpr std::size_t max_call_arguments = 32;
    // Loads AAPCS32 integer arguments: the first four in r0-r3, the rest as
    // consecutive words at the new sp (8-byte aligned, below stack_top). 64-bit
    // values are passed as two words by the caller. False if the stack words are
    // not writable guest memory; sp is then unchanged and nothing is written.
    [[nodiscard]] bool load_arguments(std::span<const std::uint32_t> arguments, std::uint32_t stack_top);
    // Scalar AAPCS32 call: up to max_call_arguments word arguments (extras on the
    // stack), with an 8-byte aligned stack. return_target is an unmapped
    // host-controlled sentinel. A stack frame that cannot be written yields a
    // memory_fault result.
    [[nodiscard]] Result call(std::uint32_t target, std::span<const std::uint32_t> arguments,
                              std::uint32_t stack_top, std::uint32_t return_target,
                              std::uint64_t budget);
};
[[nodiscard]] const char* event_name(Event event) noexcept;
}
