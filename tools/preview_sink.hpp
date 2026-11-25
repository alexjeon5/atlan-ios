// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include "atlan/gl_context.hpp"
#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

namespace atlan::tools {
// A rough software picture of what the guest draws, for looking at screens on a host without a GPU. It does not run
// the shaders: a draw is placed with its first "projection" mat4 uniform (else its first mat4) applied to the
// "pos" attribute, textured with its first sampler through the "tex" attribute, tinted with a vec4 uniform named
// "color", and blended with source alpha when blending is on. No depth test, nearest sampling. Flat 2D screens come
// out recognisable; 3D scenes only roughly. Every `every`-th presented frame is written as DIR/frame_NNNNN.ppm.
// `next` (optional) sees the same calls.
class PreviewGlSink final : public GlSink {
public:
    PreviewGlSink(std::uint32_t width, std::uint32_t height, std::string directory, std::uint32_t every, GlSink* next)
        : width_(width), height_(height), directory_(std::move(directory)), every_(std::max<std::uint32_t>(every, 1)), next_(next),
          pixels_(std::size_t{width} * height * 4, 0.0f) {}

    void clear(const GlClear& c) override {
        if (c.color)
            for (std::size_t i = 0; i < pixels_.size(); ++i) pixels_[i] = c.rgba[i % 4];
        if (next_) next_->clear(c);
    }
    void draw(const GlDraw& d) override {
        rasterize(d);
        if (next_) next_->draw(d);
    }
    void present() override {
        if (frame_ % every_ == 0) {
            char name[64];
            std::snprintf(name, sizeof name, "/frame_%05u.ppm", frame_);
            if (std::FILE* out = std::fopen((directory_ + name).c_str(), "wb")) {
                std::fprintf(out, "P6\n%u %u\n255\n", width_, height_);
                std::vector<unsigned char> row(std::size_t{width_} * 3);
                for (std::uint32_t y = 0; y < height_; ++y) {
                    for (std::uint32_t x = 0; x < width_; ++x)
                        for (unsigned c = 0; c < 3; ++c)
                            row[x * 3 + c] = static_cast<unsigned char>(std::clamp(at(x, y)[c], 0.0f, 1.0f) * 255.0f + 0.5f);
                    std::fwrite(row.data(), 1, row.size(), out);
                }
                std::fclose(out);
            }
        }
        ++frame_;
        if (next_) next_->present();
    }
    // RGBA8 rows from the bottom up, as glReadPixels returns them.
    bool read_pixels(std::int32_t x, std::int32_t y, std::uint32_t width, std::uint32_t height, std::vector<std::byte>& out) override {
        if (next_) { std::vector<std::byte> ignored; (void)next_->read_pixels(x, y, width, height, ignored); }
        out.assign(std::size_t{width} * height * 4, std::byte{0});
        for (std::uint32_t row = 0; row < height; ++row) {
            const std::int64_t from_bottom = std::int64_t{y} + row;
            if (from_bottom < 0 || from_bottom >= height_) continue;
            for (std::uint32_t column = 0; column < width; ++column) {
                const std::int64_t px = std::int64_t{x} + column;
                if (px < 0 || px >= width_) continue;
                const float* p = at(static_cast<std::uint32_t>(px), height_ - 1 - static_cast<std::uint32_t>(from_bottom));
                for (unsigned c = 0; c < 4; ++c)
                    out[(std::size_t{row} * width + column) * 4 + c] = std::byte(static_cast<unsigned char>(std::clamp(p[c], 0.0f, 1.0f) * 255.0f + 0.5f));
            }
        }
        return true;
    }

private:
    [[nodiscard]] float* at(std::uint32_t x, std::uint32_t y) { return pixels_.data() + (std::size_t{y} * width_ + x) * 4; }
    static bool contains(std::string name, const char* word) {
        for (auto& c : name) if (c >= 'A' && c <= 'Z') c = static_cast<char>(c + 32);
        return name.find(word) != std::string::npos;
    }
    void rasterize(const GlDraw& d) {
        if (!d.program || d.primitive == GlPrimitive::points || d.primitive == GlPrimitive::lines || d.primitive == GlPrimitive::line_strip) return;
        const auto& linked = d.program->linked;
        std::array<float, 16> matrix{1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1};
        std::array<float, 4> tint{1, 1, 1, 1};
        const glsl::UniformSlot* chosen = nullptr;
        for (const auto& slot : linked.uniforms) {
            const bool mat4 = slot.type.array == 0 && slot.type.rows == 4 && slot.type.cols == 4;
            if (mat4 && (!chosen || (contains(slot.name, "projection") && !contains(chosen->name, "projection")))) chosen = &slot;
            if (slot.type.array == 0 && slot.type.rows == 4 && slot.type.cols == 1 && slot.type.base == glsl::Base::float_ && contains(slot.name, "color") &&
                slot.name.size() == 5 && slot.offset + 16 <= d.uniforms.size())
                std::memcpy(tint.data(), d.uniforms.data() + slot.offset, 16);
        }
        if (chosen && chosen->offset + 64 <= d.uniforms.size()) std::memcpy(matrix.data(), d.uniforms.data() + chosen->offset, 64);
        const GlAttributeStream* positions = nullptr;
        const GlAttributeStream* coordinates = nullptr;
        for (std::size_t i = 0; i < linked.attributes.size() && i < d.streams.size(); ++i) {
            if (!positions && contains(linked.attributes[i].name, "pos")) positions = &d.streams[i];
            if (!coordinates && contains(linked.attributes[i].name, "tex") && d.streams[i].components >= 2) coordinates = &d.streams[i];
        }
        if (!positions || positions->components < 2) return;
        const GlTextureImage* image = !d.textures.empty() && d.textures[0].image ? d.textures[0].image.get() : nullptr;
        if (!d.textures.empty() && !image) return;  // an incomplete texture samples as transparent black here
        const float vx = static_cast<float>(d.state.viewport[0]), vy = static_cast<float>(d.state.viewport[1]);
        const float vw = d.state.viewport[2] > 0 ? static_cast<float>(d.state.viewport[2]) : static_cast<float>(width_);
        const float vh = d.state.viewport[3] > 0 ? static_cast<float>(d.state.viewport[3]) : static_cast<float>(height_);
        struct Vertex { float x, y, u, v; bool ok; };
        const auto vertex = [&](std::uint32_t index) -> Vertex {
            if (index >= d.vertex_count) return {0, 0, 0, 0, false};
            const float* p = positions->values.data() + std::size_t{index} * positions->components;
            const float in[4] = {p[0], p[1], positions->components > 2 ? p[2] : 0.0f, positions->components > 3 ? p[3] : 1.0f};
            float clip[4];
            for (unsigned r = 0; r < 4; ++r) clip[r] = matrix[r] * in[0] + matrix[4 + r] * in[1] + matrix[8 + r] * in[2] + matrix[12 + r] * in[3];
            if (!(clip[3] > 1e-6f)) return {0, 0, 0, 0, false};
            Vertex out{};
            out.x = vx + (clip[0] / clip[3] + 1) * 0.5f * vw;
            out.y = static_cast<float>(height_) - (vy + (clip[1] / clip[3] + 1) * 0.5f * vh);
            if (coordinates) { const float* t = coordinates->values.data() + std::size_t{index} * coordinates->components; out.u = t[0]; out.v = t[1]; }
            out.ok = std::isfinite(out.x) && std::isfinite(out.y);
            return out;
        };
        const bool strip = d.primitive == GlPrimitive::triangle_strip;
        for (std::size_t i = 0; i + 2 < d.indices.size(); i += strip ? 1 : 3) {
            const Vertex a = vertex(d.indices[i]), b = vertex(d.indices[i + 1]), c = vertex(d.indices[i + 2]);
            if (!a.ok || !b.ok || !c.ok) continue;
            const float area = (b.x - a.x) * (c.y - a.y) - (b.y - a.y) * (c.x - a.x);
            if (std::fabs(area) < 1e-6f) continue;
            const auto x0 = static_cast<std::int64_t>(std::floor(std::max(0.0f, std::min({a.x, b.x, c.x}))));
            const auto x1 = static_cast<std::int64_t>(std::ceil(std::min(static_cast<float>(width_), std::max({a.x, b.x, c.x}))));
            const auto y0 = static_cast<std::int64_t>(std::floor(std::max(0.0f, std::min({a.y, b.y, c.y}))));
            const auto y1 = static_cast<std::int64_t>(std::ceil(std::min(static_cast<float>(height_), std::max({a.y, b.y, c.y}))));
            for (std::int64_t y = y0; y < y1; ++y) {
                for (std::int64_t x = x0; x < x1; ++x) {
                    const float px = static_cast<float>(x) + 0.5f, py = static_cast<float>(y) + 0.5f;
                    const float w0 = ((b.x - px) * (c.y - py) - (b.y - py) * (c.x - px)) / area;
                    const float w1 = ((c.x - px) * (a.y - py) - (c.y - py) * (a.x - px)) / area;
                    const float w2 = 1 - w0 - w1;
                    if (w0 < 0 || w1 < 0 || w2 < 0) continue;
                    std::array<float, 4> color = tint;
                    if (image && image->width != 0 && image->height != 0) {
                        float u = w0 * a.u + w1 * b.u + w2 * c.u, v = w0 * a.v + w1 * b.v + w2 * c.v;
                        u -= std::floor(u); v -= std::floor(v);
                        const auto tx = std::min<std::uint32_t>(image->width - 1, static_cast<std::uint32_t>(u * static_cast<float>(image->width)));
                        const auto ty = std::min<std::uint32_t>(image->height - 1, static_cast<std::uint32_t>(v * static_cast<float>(image->height)));
                        const auto* texel = image->rgba.data() + (std::size_t{ty} * image->width + tx) * 4;
                        for (unsigned k = 0; k < 4; ++k) color[k] *= static_cast<float>(texel[k]) / 255.0f;
                    }
                    float* out = at(static_cast<std::uint32_t>(x), static_cast<std::uint32_t>(y));
                    const float alpha = d.state.blend ? color[3] : 1.0f;
                    for (unsigned k = 0; k < 4; ++k) out[k] = color[k] * alpha + out[k] * (1 - alpha);
                }
            }
        }
    }
    std::uint32_t width_, height_;
    std::string directory_;
    std::uint32_t every_, frame_{};
    GlSink* next_;
    std::vector<float> pixels_;
};
}
