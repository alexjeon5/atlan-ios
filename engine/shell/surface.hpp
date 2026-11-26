// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include "shell/render_config.hpp"
#include <atomic>
#include <cstdint>

namespace atlan::shell {
// Size of the host render surface in native pixels. The UI thread publishes it; the guest thread reads it
// (for example to answer a size query). A packed 64-bit word keeps width and height consistent.
class SurfaceState {
public:
    void set(PixelSize size) noexcept {
        packed_.store((std::uint64_t{size.width} << 32) | size.height, std::memory_order_release);
        generation_.fetch_add(1, std::memory_order_acq_rel);
    }
    [[nodiscard]] PixelSize get() const noexcept {
        const auto v = packed_.load(std::memory_order_acquire);
        return {static_cast<std::uint32_t>(v >> 32), static_cast<std::uint32_t>(v & 0xffffffffU)};
    }
    [[nodiscard]] std::uint32_t generation() const noexcept { return generation_.load(std::memory_order_acquire); }
private:
    std::atomic<std::uint64_t> packed_{0};
    std::atomic<std::uint32_t> generation_{0};
};
}
