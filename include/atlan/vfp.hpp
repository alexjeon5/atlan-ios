// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include "atlan/cpu_state.hpp"
#include "atlan/memory.hpp"

namespace atlan::vfp {
// Scalar VFPv2/v3 (single and double precision) for A32 words. The Kernarium
// integer core does not execute these, so the runtime routes them here first.
struct Execution {
    enum class Kind : std::uint8_t { not_vfp, retired, fault, unsupported } kind{Kind::not_vfp};
    std::uint32_t address{};  // fault address
    MemoryAccess access{MemoryAccess::read};
    bool alignment{};
    const char* reason{};     // unsupported: why, as a static string
};
// Executes one A32 word at `pc` when it is a VFP instruction whose condition
// is evaluated here. cpu.pc advances only for Kind::retired.
[[nodiscard]] Execution execute_a32(CpuState& cpu, Memory& memory, std::uint32_t word,
                                    std::uint32_t pc) noexcept;
}
