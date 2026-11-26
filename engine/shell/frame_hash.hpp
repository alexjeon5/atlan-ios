// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include "atlan/gles.hpp"
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <map>
#include <unordered_map>
#include <vector>

namespace atlan::shell {
// Passes every call on to `inner` and hashes each call of a frame, so the interactive loop can tell a frame that
// drew what the frame before it drew: the same calls, of which at most `changed_calls_allowed` differ (a small
// animation such as a turning 3D landmark or a clock, which then moves at the slower still rate). Object names and
// client pointers are not hashed as such: a guest that makes a new texture or buffer each frame with the same
// contents, or draws from a client array at a new heap address, still draws the same picture. A texture or buffer is hashed by what was last
// uploaded to it (and its parameters), and a client-array pointer not at all (the draw call carries its data).
class FrameHashBackend final : public GlBackend {
public:
    explicit FrameHashBackend(GlBackend& inner) : inner_(inner) {}
    GlResult execute(const GlCall& call) override {
        if (call.op == GlOp::swap_buffers) {
            same_ = false;
            if (frames_ > 0 && calls_.size() == previous_.size()) {
                std::size_t changed = 0;
                for (std::size_t i = 0; i < calls_.size() && changed <= changed_calls_allowed; ++i) changed += calls_[i] != previous_[i];
                same_ = changed <= changed_calls_allowed;
            }
            previous_.swap(calls_);
            calls_.clear();
            ++frames_;
        } else {
            hash_call(call);
        }
        return inner_.execute(call);
    }
    // Whether the last swapped frame drew what the one before it drew (see above).
    [[nodiscard]] bool last_frame_same() const noexcept { return same_; }

private:
    static constexpr std::uint64_t basis = 0xcbf29ce484222325ULL;
    static constexpr std::size_t changed_calls_allowed = 16;
    static constexpr std::uint32_t element_buffer = 0x8893, texture0 = 0x84C0;
    static std::uint64_t step(std::uint64_t h, std::uint64_t value) noexcept {
        h = (h ^ value) * 0x100000001b3ULL;
        return h ^ (h >> 29);
    }
    static std::uint64_t bytes(std::uint64_t h, const void* data, std::size_t size) noexcept {
        const auto* p = static_cast<const unsigned char*>(data);
        h = step(h, size);
        for (; size >= 8; size -= 8, p += 8) {
            std::uint64_t word;
            std::memcpy(&word, p, 8);
            h = step(h, word);
        }
        std::uint64_t tail = 0;
        if (size != 0) std::memcpy(&tail, p, size);
        return step(h, tail);
    }
    // The call's own arguments and payload, with `a` as given (names and pointers already replaced).
    static std::uint64_t content(const GlCall& call, const std::array<std::uint32_t, 8>& a) noexcept {
        std::uint64_t h = step(basis, static_cast<std::uint64_t>(call.op));
        h = bytes(h, a.data(), sizeof a);
        h = bytes(h, call.f.data(), sizeof call.f);
        h = bytes(h, call.data.data(), call.data.size());
        h = bytes(h, call.text.data(), call.text.size());
        for (const auto& attrib : call.attribs) {
            h = step(h, std::uint64_t{attrib.index} | std::uint64_t{attrib.size} << 8 | std::uint64_t{attrib.type} << 16 |
                            std::uint64_t{attrib.stride} << 32 | std::uint64_t{attrib.normalized} << 63);
            h = bytes(h, attrib.bytes.data(), attrib.bytes.size());
        }
        return step(h, call.vertex_count);
    }
    std::uint64_t texture_hash(std::uint32_t name) const {
        const auto found = textures_.find(name);
        if (found == textures_.end()) return name == 0 ? 0 : 1;
        std::uint64_t h = found->second.image;
        for (const auto& [parameter, value] : found->second.parameters) h = step(step(h, parameter), value);
        return h;
    }
    std::uint64_t buffer_hash(std::uint32_t name) const {
        const auto found = buffers_.find(name);
        return found == buffers_.end() ? name == 0 ? 0 : 1 : found->second;
    }
    std::uint32_t& bound_texture() { return bound_textures_[active_unit_ % bound_textures_.size()]; }
    void hash_call(const GlCall& call) {
        auto a = call.a;
        switch (call.op) {
        case GlOp::active_texture: active_unit_ = call.a[0] - texture0; break;
        case GlOp::bind_texture:
            bound_texture() = call.a[1];
            a[1] = static_cast<std::uint32_t>(texture_hash(call.a[1]));
            break;
        case GlOp::tex_image_2d:
        case GlOp::compressed_tex_image_2d:
            textures_[bound_texture()].image = content(call, a);
            break;
        case GlOp::tex_parameter_i:  // target pname param (glTexParameterf: the param in f[0])
        case GlOp::tex_parameter_f: {
            std::uint32_t value = call.a[2];
            if (call.op == GlOp::tex_parameter_f) std::memcpy(&value, &call.f[0], 4);
            textures_[bound_texture()].parameters[call.a[1]] = value;
            break;
        }
        case GlOp::bind_buffer:
            (call.a[0] == element_buffer ? element_buffer_ : array_buffer_) = call.a[1];
            a[1] = static_cast<std::uint32_t>(buffer_hash(call.a[1]));
            break;
        case GlOp::buffer_data:
            buffers_[call.a[0] == element_buffer ? element_buffer_ : array_buffer_] = content(call, a);
            break;
        case GlOp::delete_texture:
            textures_.erase(call.a[0]);
            a[0] = 0;
            break;
        case GlOp::delete_buffer:
            buffers_.erase(call.a[0]);
            a[0] = 0;
            break;
        case GlOp::vertex_attrib_pointer:  // index size type normalized stride pointer array-buffer
            if (call.a[6] == 0) a[5] = 0;
            a[6] = static_cast<std::uint32_t>(buffer_hash(call.a[6]));
            break;
        case GlOp::draw_elements:  // mode count type pointer element-buffer
            if (call.a[4] == 0) a[3] = 0;
            a[4] = static_cast<std::uint32_t>(buffer_hash(call.a[4]));
            break;
        default: break;
        }
        calls_.push_back(content(call, a));  // `id` (a name the service handed out) is left out
    }
    GlBackend& inner_;
    std::vector<std::uint64_t> calls_, previous_;  // per-call hashes of the frame being drawn and the last one
    std::uint64_t frames_{};
    bool same_{};
    std::uint32_t active_unit_{}, array_buffer_{}, element_buffer_{};
    std::array<std::uint32_t, 32> bound_textures_{};
    struct Texture {
        std::uint64_t image{};
        std::map<std::uint32_t, std::uint32_t> parameters;  // the last value set for each parameter
    };
    std::unordered_map<std::uint32_t, Texture> textures_;
    std::unordered_map<std::uint32_t, std::uint64_t> buffers_;
};
}
