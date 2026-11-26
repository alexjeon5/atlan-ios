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

// Where a surface is shown inside a view (any unit, origin top-left): centred, as large as fits with its aspect
// ratio kept. An aspect mismatch under 1% is absorbed by filling the view. An unknown surface covers the view.
struct ViewRect { double x{}, y{}, width{}, height{}; };
[[nodiscard]] constexpr ViewRect fit_rect(double view_width, double view_height, PixelSize surface) noexcept {
    if (!(view_width > 0) || !(view_height > 0) || surface.width == 0 || surface.height == 0) return {0, 0, view_width, view_height};
    const double scale = std::min(view_width / surface.width, view_height / surface.height);
    const double width = surface.width * scale, height = surface.height * scale;
    if (width >= view_width * 0.99 && height >= view_height * 0.99) return {0, 0, view_width, view_height};
    return {(view_width - width) / 2, (view_height - height) / 2, width, height};
}
}
