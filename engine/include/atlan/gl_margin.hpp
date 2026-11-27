// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include "atlan/gles.hpp"
#include <array>
#include <atomic>
#include <cstdint>
#include <map>
#include <set>
#include <string>
#include <vector>

namespace atlan {
// GlBackend decorator that shows the guest's frame inside a wider target: `left` and `right` extra columns of
// target pixels beside the guest's own surface. Everything the guest draws keeps its place (guest pixel x lands on
// target pixel x + left), and a draw whose viewport reaches the guest's left or right edge also fills that side's
// margin with what it puts past the edge, the way a wider screen would show it. A scene the guest lays out for its
// surface (a map) so runs on under the margins, while anything placed inside the surface (buttons, text) stays.
// A draw that spans the guest's whole width (sky, a full-screen picture, a bar) is drawn once more into each margin,
// its outermost column drawn out over it, so the margin continues it; nothing on the surface moves or stretches.
// That needs the draw's positions, so the app names its position attribute and the matrix that takes it to clip
// space (set_scene_names); a panel that only touches one edge is left alone.
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
    // Before the first call: the vertex attribute holding positions and the mat4 uniform that maps them to clip
    // space (gl_Position = matrix * position). Without them no draw is stretched over the margins.
    void set_scene_names(std::string position_attribute, std::string clip_matrix);
    // Guest thread, like every GlBackend call.
    GlResult execute(const GlCall& call) override;
    // A vertex shader with the transform appended (its main renamed); the source unchanged when it has no main.
    static std::string patch_vertex_shader(const std::string& source);
    static constexpr const char* uniform_name = "atlan_margin_x";  // vec2: clip x' = x * [0] + w * [1]
private:
    void apply_viewport();
    void set_inner_viewport(std::int32_t x, std::int32_t width);
    void send_transform(const std::array<float, 2>& value);
    GlResult draw(const GlCall& call);
    struct Edges { bool left{}, right{}; };
    [[nodiscard]] Edges backdrop_edges(const GlCall& draw);  // the margins a backdrop draw continues into
    std::pair<float, float> band_{9, 9};  // NDC y extent of the last band backdrop
    GlBackend& inner_;
    std::atomic<std::uint64_t> pending_{0};  // (width << 32) | (left << 16) | right; 0 = unchanged
    std::uint32_t width_{}, left_{}, right_{};
    std::int32_t vx_{}, vy_{}, vw_{}, vh_{};  // the guest's viewport
    bool viewport_set_{};
    std::array<float, 2> transform_{1, 0};
    std::int32_t grow_left_{}, grow_right_{};  // margin columns the current viewport is widened by
    std::uint32_t program_{};
    std::set<std::uint32_t> vertex_shaders_;
    std::map<std::uint32_t, std::int32_t> locations_;          // program -> uniform location (-1: none)
    std::string position_name_, matrix_name_;
    struct Scene { std::int32_t position{-1}, matrix{-1}; bool has_matrix{}; std::array<float, 16> clip{}; };
    std::map<std::uint32_t, Scene> scenes_;                    // program -> position attribute and its matrix
    struct Pointer { std::uint32_t size{}, type{}, stride{}, offset{}, buffer{}; bool normalized{}; };
    std::map<std::uint32_t, Pointer> pointers_;                // attribute index -> its buffer source (buffer 0: client)
    std::uint32_t array_buffer_{};
    std::map<std::uint32_t, std::vector<std::byte>> buffers_;  // small array buffers' contents, for positions
    std::map<std::uint32_t, std::array<float, 2>> sent_;       // program -> transform last set
};
}
