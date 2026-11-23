// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include <algorithm>
#include <cstdint>

namespace atlan::shell {
struct PixelSize { std::uint32_t width{}, height{}; };

// Internal render size from the view's size in points. The default scale of 1.0
// is the device's native pixel resolution; a lower scale (for heat) shrinks the
// internal surface that is later stretched to the screen.
[[nodiscard]] constexpr PixelSize render_size(double width_points, double height_points,
                                              double native_scale, double render_scale = 1.0) noexcept {
    const double factor = native_scale * std::clamp(render_scale, 0.25, 1.0);
    const auto px = [factor](double points) {
        return static_cast<std::uint32_t>(std::max(1.0, points * factor + 0.5));
    };
    return {px(width_points), px(height_points)};
}
}
