// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include <array>
#include <cstddef>
#include <cstdint>

namespace atlan {
struct alignas(16) VectorRegister {
    std::array<std::byte, 16> bytes{};
};

// R0-R14 are zero extended; PC uses the imported AArch32/Thumb key format.
// Vector storage is reserved for the semantic interface; FP/SIMD is unsupported.
struct CpuState {
    std::array<std::uint64_t, 16> registers{};
    std::uint64_t pc{};
    std::uint32_t nzcv{};
    std::uint32_t fpscr{};  // VFP status and control; see docs/vfp.md
    std::array<VectorRegister, 16> vectors{};
    std::uint64_t tls{};
    std::uint32_t a32_ge_mask{};
    std::uint32_t a32_tpidruro{};
    std::uint8_t a32_q{};
    std::uint8_t a32_itstate{};
    std::uint8_t a32_cp15ben{};
    std::uint64_t exclusive_address{~std::uint64_t{0}};
    std::uintptr_t exclusive_key{};
    std::uint64_t exclusive_value{};
    std::uint64_t exclusive_value2{};
    std::uint64_t exclusive_version{};
    std::uint8_t exclusive_size{};

    void clear_exclusive_monitor() noexcept {
        exclusive_address = ~std::uint64_t{0};
        exclusive_key = 0;
        exclusive_value = exclusive_value2 = exclusive_version = 0;
        exclusive_size = 0;
    }
};
}
