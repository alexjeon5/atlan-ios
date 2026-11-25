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

// Guest screen size that fills a host surface without distortion: the configured short side is kept and the long
// side follows the host's aspect ratio, rounded to a multiple of 8. Unknown sizes return `configured` unchanged.
[[nodiscard]] constexpr PixelSize fill_screen(PixelSize configured, PixelSize host) noexcept {
    if (configured.width == 0 || configured.height == 0 || host.width == 0 || host.height == 0) return configured;
    const std::uint32_t short_side = std::min(configured.width, configured.height);
    const bool landscape = host.width >= host.height;
    const double aspect = landscape ? static_cast<double>(host.width) / host.height : static_cast<double>(host.height) / host.width;
    const auto long_side = std::clamp<std::uint32_t>(static_cast<std::uint32_t>(short_side * aspect / 8.0 + 0.5) * 8, short_side, 8192);
    return landscape ? PixelSize{long_side, short_side} : PixelSize{short_side, long_side};
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
