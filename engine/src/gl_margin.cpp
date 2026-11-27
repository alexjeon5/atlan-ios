// SPDX-License-Identifier: GPL-2.0-or-later
#include "atlan/gl_margin.hpp"
#include <algorithm>
#include <cctype>
#include <cstring>

namespace atlan {
namespace {
constexpr std::uint32_t vertex_shader = 0x8B31;  // GL_VERTEX_SHADER
bool word_char(char c) { return std::isalnum(static_cast<unsigned char>(c)) || c == '_'; }
bool space(char c) { return std::isspace(static_cast<unsigned char>(c)) != 0; }
}

std::string MarginGlBackend::patch_vertex_shader(const std::string& source) {
    // The first "void main (" outside comments.
    for (std::size_t i = 0; i < source.size();) {
        if (source.compare(i, 2, "//") == 0) { i = source.find('\n', i); if (i == std::string::npos) break; continue; }
        if (source.compare(i, 2, "/*") == 0) { i = source.find("*/", i + 2); if (i == std::string::npos) break; i += 2; continue; }
        const bool at_void = source.compare(i, 4, "void") == 0 && (i == 0 || !word_char(source[i - 1])) &&
                             i + 4 < source.size() && !word_char(source[i + 4]);
        if (!at_void) { ++i; continue; }
        std::size_t name = i + 4;
        while (name < source.size() && space(source[name])) ++name;
        std::size_t paren = name + 4;
        while (paren < source.size() && space(source[paren])) ++paren;
        if (source.compare(name, 4, "main") != 0 || paren >= source.size() || source[paren] != '(') { i += 4; continue; }
        const std::string u = uniform_name;
        // A vertex on the edge of the guest's viewport (clip x = -w or +w) goes to the edge of the widened one when
        // that side is widened; every other vertex keeps its guest pixel.
        return source.substr(0, name) + "atlan_guest_main" + source.substr(name + 4) +
               "\nuniform highp vec4 " + u + ";\n"
               "void main()\n{\n"
               "    atlan_guest_main();\n"
               "    highp float atlan_x = gl_Position.x;\n"
               "    highp float atlan_w = gl_Position.w;\n"
               "    highp float atlan_near = abs(atlan_w) * 0.0001;\n"
               "    if (" + u + ".z != 0.0 && abs(atlan_x + atlan_w) <= atlan_near) gl_Position.x = -atlan_w;\n"
               "    else if (" + u + ".w != 0.0 && abs(atlan_x - atlan_w) <= atlan_near) gl_Position.x = atlan_w;\n"
               "    else gl_Position.x = atlan_x * " + u + ".x + atlan_w * " + u + ".y;\n"
               "}\n";
    }
    return source;
}

void MarginGlBackend::set_layout(std::uint32_t guest_width, std::uint32_t left, std::uint32_t right) noexcept {
    if (guest_width == 0) return;
    left = std::min<std::uint32_t>(left, 0xFFFF);
    right = std::min<std::uint32_t>(right, 0xFFFF);
    pending_.store((std::uint64_t{guest_width} << 32) | (std::uint64_t{left} << 16) | right, std::memory_order_release);
}

void MarginGlBackend::apply_viewport() {
    const auto edge = static_cast<std::int32_t>(width_);
    const std::int32_t grow_left = vx_ <= 0 ? static_cast<std::int32_t>(left_) : 0;
    const std::int32_t grow_right = width_ != 0 && vx_ + vw_ >= edge ? static_cast<std::int32_t>(right_) : 0;
    const std::int32_t x = vx_ + static_cast<std::int32_t>(left_) - grow_left;
    const std::int32_t width = vw_ + grow_left + grow_right;
    // Guest NDC x lands on pixel vx + left + (x + 1) / 2 * vw inside the widened viewport when
    // x' = x * vw / width + (grow_left - grow_right) / width.
    if (vw_ > 0 && width > 0) {
        transform_ = {static_cast<float>(vw_) / static_cast<float>(width),
                      static_cast<float>(grow_left - grow_right) / static_cast<float>(width),
                      grow_left > 0 ? 1.0F : 0.0F, grow_right > 0 ? 1.0F : 0.0F};
    } else {
        transform_ = {1, 0, 0, 0};
    }
    GlCall call;
    call.op = GlOp::viewport;
    call.a[0] = static_cast<std::uint32_t>(x);
    call.a[1] = static_cast<std::uint32_t>(vy_);
    call.a[2] = static_cast<std::uint32_t>(width);
    call.a[3] = static_cast<std::uint32_t>(vh_);
    inner_.execute(call);
}

void MarginGlBackend::update_uniform() {
    const auto location = locations_.find(program_);
    if (location == locations_.end() || location->second < 0) return;
    const auto sent = sent_.find(program_);
    if (sent != sent_.end() && sent->second == transform_) return;
    GlCall call;
    call.op = GlOp::uniform_4fv;
    call.a[0] = static_cast<std::uint32_t>(location->second);
    call.a[1] = 1;
    call.data.resize(sizeof transform_);
    std::memcpy(call.data.data(), transform_.data(), sizeof transform_);
    inner_.execute(call);
    sent_[program_] = transform_;
}

GlResult MarginGlBackend::execute(const GlCall& c) {
    if (const auto layout = pending_.exchange(0, std::memory_order_acq_rel); layout != 0) {
        width_ = static_cast<std::uint32_t>(layout >> 32);
        left_ = static_cast<std::uint32_t>(layout >> 16) & 0xFFFF;
        right_ = static_cast<std::uint32_t>(layout) & 0xFFFF;
        if (viewport_set_) apply_viewport();
    }
    switch (c.op) {
        case GlOp::create_shader:
            if (c.a[0] == vertex_shader) vertex_shaders_.insert(c.id);
            break;
        case GlOp::delete_shader: vertex_shaders_.erase(c.a[0]); break;
        case GlOp::shader_source:
            if (vertex_shaders_.contains(c.a[0])) {
                GlCall patched = c;
                patched.text = patch_vertex_shader(c.text);
                return inner_.execute(patched);
            }
            break;
        case GlOp::link_program: {
            auto result = inner_.execute(c);
            GlCall query;
            query.op = GlOp::get_uniform_location;
            query.a[0] = c.a[0];
            query.text = uniform_name;
            locations_[c.a[0]] = static_cast<std::int32_t>(inner_.execute(query).value);
            sent_.erase(c.a[0]);  // linking resets the program's uniforms
            return result;
        }
        case GlOp::delete_program:
            locations_.erase(c.a[0]);
            sent_.erase(c.a[0]);
            break;
        case GlOp::use_program: program_ = c.a[0]; break;
        case GlOp::viewport:
            vx_ = static_cast<std::int32_t>(c.a[0]);
            vy_ = static_cast<std::int32_t>(c.a[1]);
            vw_ = static_cast<std::int32_t>(c.a[2]);
            vh_ = static_cast<std::int32_t>(c.a[3]);
            viewport_set_ = true;
            apply_viewport();
            return {};
        case GlOp::read_pixels:
            if (left_ != 0) {
                GlCall shifted = c;
                shifted.a[0] = c.a[0] + left_;
                return inner_.execute(shifted);
            }
            break;
        case GlOp::draw_arrays:
        case GlOp::draw_elements: update_uniform(); break;
        default: break;
    }
    return inner_.execute(c);
}
}
