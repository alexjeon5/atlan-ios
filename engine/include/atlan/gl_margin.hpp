// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include "atlan/gles.hpp"
#include <array>
#include <atomic>
#include <cstdint>
#include <map>
#include <set>
#include <string>

namespace atlan {
// GlBackend decorator that shows the guest's frame inside a wider target: `left` and `right` extra columns of
// target pixels beside the guest's own surface. Everything the guest draws keeps its place (guest pixel x lands on
// target pixel x + left), and a draw whose viewport reaches the guest's left or right edge also fills that side's
// margin with what it puts past the edge, the way a wider screen would show it. A scene the guest lays out for its
// surface (a map) so runs on under the margins, while anything placed inside the surface (buttons, text) stays.
// Vertices exactly on a widened edge move to the margin's outer edge, so a backdrop that spans the guest's whole
// width (sky, a full-screen picture) spans the margin too.
//
// Each vertex shader is rewritten to end with a clip-space x transform taken from a host uniform; viewports that
// touch an edge are widened over the margin and the uniform keeps the guest's pixels in place. Read-backs shift by
// `left`. The wrapped backend's surface must be guest width + left + right wide. With both margins 0 the guest's
// frame comes out unchanged.
class MarginGlBackend final : public GlBackend {
public:
    explicit MarginGlBackend(GlBackend& inner) : inner_(inner) {}
    // Any thread; applied before the guest's next call. `guest_width` is the guest's surface width in pixels.
    void set_layout(std::uint32_t guest_width, std::uint32_t left, std::uint32_t right) noexcept;
    // Guest thread, like every GlBackend call.
    GlResult execute(const GlCall& call) override;
    // A vertex shader with the transform appended (its main renamed); the source unchanged when it has no main.
    static std::string patch_vertex_shader(const std::string& source);
    static constexpr const char* uniform_name = "atlan_margin_x";  // vec4: x scale, w offset, snap left, snap right
private:
    void apply_viewport();
    void update_uniform();
    GlBackend& inner_;
    std::atomic<std::uint64_t> pending_{0};  // (width << 32) | (left << 16) | right; 0 = unchanged
    std::uint32_t width_{}, left_{}, right_{};
    std::int32_t vx_{}, vy_{}, vw_{}, vh_{};  // the guest's viewport
    bool viewport_set_{};
    std::array<float, 4> transform_{1, 0, 0, 0};
    std::uint32_t program_{};
    std::set<std::uint32_t> vertex_shaders_;
    std::map<std::uint32_t, std::int32_t> locations_;          // program -> uniform location (-1: none)
    std::map<std::uint32_t, std::array<float, 4>> sent_;       // program -> transform last set
};
}
