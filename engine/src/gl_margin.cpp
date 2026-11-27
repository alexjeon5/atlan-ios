// SPDX-License-Identifier: GPL-2.0-or-later
#include "atlan/gl_margin.hpp"
#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstring>

namespace atlan {
namespace {
constexpr std::uint32_t vertex_shader = 0x8B31;  // GL_VERTEX_SHADER
bool word_char(char c) { return std::isalnum(static_cast<unsigned char>(c)) || c == '_'; }
bool space(char c) { return std::isspace(static_cast<unsigned char>(c)) != 0; }
// GL_BYTE, GL_UNSIGNED_BYTE, GL_SHORT, GL_UNSIGNED_SHORT, GL_FLOAT; 0 for the rest.
std::size_t component_size(std::uint32_t type) {
    switch (type) {
        case 0x1400: case 0x1401: return 1;
        case 0x1402: case 0x1403: return 2;
        case 0x1406: return 4;
    }
    return 0;
}
float component_value(std::uint32_t type, bool normalized, const std::byte* at) {
    switch (type) {
        case 0x1400: { std::int8_t v; std::memcpy(&v, at, 1); return normalized ? std::max(v / 127.0F, -1.0F) : v; }
        case 0x1401: { std::uint8_t v; std::memcpy(&v, at, 1); return normalized ? v / 255.0F : v; }
        case 0x1402: { std::int16_t v; std::memcpy(&v, at, 2); return normalized ? std::max(v / 32767.0F, -1.0F) : v; }
        case 0x1403: { std::uint16_t v; std::memcpy(&v, at, 2); return normalized ? v / 65535.0F : v; }
    }
    float v;
    std::memcpy(&v, at, 4);
    return v;
}
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
        return source.substr(0, name) + "atlan_guest_main" + source.substr(name + 4) +
               "\nuniform highp vec2 " + u + ";\n"
               "void main()\n{\n"
               "    atlan_guest_main();\n"
               "    gl_Position.x = gl_Position.x * " + u + ".x + gl_Position.w * " + u + ".y;\n"
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

void MarginGlBackend::set_scene_names(std::string position_attribute, std::string clip_matrix) {
    position_name_ = std::move(position_attribute);
    matrix_name_ = std::move(clip_matrix);
}

MarginGlBackend::Edges MarginGlBackend::backdrop_edges(const GlCall& draw) {
    const auto scene = scenes_.find(program_);
    if (scene == scenes_.end() || scene->second.position < 0 || !scene->second.has_matrix) return {};
    // The positions: gathered client-side bytes, or a small array buffer this backend has seen uploaded.
    const auto index = static_cast<std::uint32_t>(scene->second.position);
    const std::vector<std::byte>* bytes = nullptr;
    std::uint32_t size = 0, type = 0, stride = 0, offset = 0;
    bool normalized = false;
    for (const auto& a : draw.attribs)
        if (a.index == index) { bytes = &a.bytes; size = a.size; type = a.type; stride = a.stride; normalized = a.normalized; }
    if (!bytes) {
        const auto pointer = pointers_.find(index);
        if (pointer == pointers_.end() || pointer->second.buffer == 0) return {};
        const auto buffer = buffers_.find(pointer->second.buffer);
        if (buffer == buffers_.end()) return {};
        bytes = &buffer->second;
        size = pointer->second.size; type = pointer->second.type; stride = pointer->second.stride; offset = pointer->second.offset;
        normalized = pointer->second.normalized;
    }
    constexpr std::size_t most = 256;  // a backdrop is a few vertices; scene geometry is left alone
    const std::size_t component = component_size(type);
    if (component == 0 || size == 0 || size > 4) return {};
    const std::size_t step = stride ? stride : size * component;
    if (bytes->size() <= offset || (bytes->size() - offset) / step > most) return {};
    const auto& m = scene->second.clip;  // column-major
    bool left = false, right = false, top = false, bottom = false;
    float low = 2, high = -2, under = 2, over = -2;  // NDC x and y extent
    for (std::size_t at = offset; at + size * component <= bytes->size(); at += step) {
        std::array<float, 4> p{0, 0, 0, 1};
        for (std::uint32_t k = 0; k < size; ++k) p[k] = component_value(type, normalized, bytes->data() + at + k * component);
        const float x = m[0] * p[0] + m[4] * p[1] + m[8] * p[2] + m[12] * p[3];
        const float w = m[3] * p[0] + m[7] * p[1] + m[11] * p[2] + m[15] * p[3];
        if (!(w > 0)) continue;
        const float y = m[1] * p[0] + m[5] * p[1] + m[9] * p[2] + m[13] * p[3];
        const float near = w * 0.001F;
        left = left || x <= -w + near;
        right = right || x >= w - near;
        top = top || y >= w - near;
        bottom = bottom || y <= -w + near;
        low = std::min(low, x / w);
        high = std::max(high, x / w);
        under = std::min(under, y / w);
        over = std::max(over, y / w);
    }
    // Across the whole width: both sides. From one side, at least a third of the width (a panel or picture there,
    // or a backdrop drawn in tiles), or a band along the top or bottom edge at the height of such a draw seen before
    // (the narrow end of a scrolling tiled sky): that side. Buttons and smaller boxes at an edge: neither.
    // A draw reaching past an edge (scene geometry, or a backdrop wider than the screen) already shows in that
    // margin through the widened viewport; only one ending on the edge is continued.
    left = left && low >= -1.01F;
    right = right && high <= 1.01F;
    if (left && right) return {true, true};
    if (!left && !right) return {};
    const bool band = top != bottom;
    const auto same = [&](const std::pair<float, float>& b) { return std::abs(under - b.first) < 0.002F && std::abs(over - b.second) < 0.002F; };
    const auto seen = band ? std::find_if(bands_.begin(), bands_.end(), same) : bands_.end();
    if (high - low < 2.0F / 3 && seen == bands_.end()) return {};
    if (band) {  // most recent first; a sky band recurs every frame and stays in the list
        if (seen != bands_.end()) bands_.erase(seen);
        bands_.insert(bands_.begin(), {under, over});
        if (bands_.size() > 8) bands_.pop_back();
    }
    return {left, right};
}

void MarginGlBackend::apply_viewport() {
    const auto edge = static_cast<std::int32_t>(width_);
    const std::int32_t grow_left = vx_ <= 0 ? static_cast<std::int32_t>(left_) : 0;
    const std::int32_t grow_right = width_ != 0 && vx_ + vw_ >= edge ? static_cast<std::int32_t>(right_) : 0;
    const std::int32_t x = vx_ + static_cast<std::int32_t>(left_) - grow_left;
    const std::int32_t width = vw_ + grow_left + grow_right;
    set_inner_viewport(x, width);
    // Guest NDC x lands on pixel vx + left + (x + 1) / 2 * vw inside the widened viewport when
    // x' = x * vw / width + (grow_left - grow_right) / width.
    if (vw_ > 0 && width > 0) {
        transform_ = {static_cast<float>(vw_) / static_cast<float>(width),
                      static_cast<float>(grow_left - grow_right) / static_cast<float>(width)};
    } else {
        transform_ = {1, 0};
    }
    grow_left_ = grow_left;
    grow_right_ = grow_right;
}

void MarginGlBackend::set_inner_viewport(std::int32_t x, std::int32_t width) {
    GlCall call;
    call.op = GlOp::viewport;
    call.a[0] = static_cast<std::uint32_t>(x);
    call.a[1] = static_cast<std::uint32_t>(vy_);
    call.a[2] = static_cast<std::uint32_t>(width);
    call.a[3] = static_cast<std::uint32_t>(vh_);
    inner_.execute(call);
}

void MarginGlBackend::send_transform(const std::array<float, 2>& value) {
    const auto location = locations_.find(program_);
    if (location == locations_.end() || location->second < 0) return;
    const auto sent = sent_.find(program_);
    if (sent != sent_.end() && sent->second == value) return;
    GlCall call;
    call.op = GlOp::uniform_2fv;
    call.a[0] = static_cast<std::uint32_t>(location->second);
    call.a[1] = 1;
    call.data.resize(sizeof value);
    std::memcpy(call.data.data(), value.data(), sizeof value);
    inner_.execute(call);
    sent_[program_] = value;
}

GlResult MarginGlBackend::draw(const GlCall& c) {
    send_transform(transform_);
    auto result = inner_.execute(c);
    if (grow_left_ == 0 && grow_right_ == 0) return result;
    const auto edges = backdrop_edges(c);
    if (!edges.left && !edges.right) return result;
    // A backdrop (across the whole width, a picture of the screen, or a band from one side such as a tiled sky):
    // draw it again into each margin it reaches, alone, its edge column drawn out over the margin. Inside a margin's own viewport, guest viewport pixel
    // gx lands on margin pixel gx * smear from that edge, so the margin shows the guest's outermost pixel.
    // The column taken is `inset` pixels in from the edge: a picture's outermost texels are often a light border.
    constexpr float smear = 50, inset = 2;
    const float guest = static_cast<float>(vw_);
    if (grow_left_ > 0 && edges.left) {
        const float margin = static_cast<float>(grow_left_);
        const float s = smear * guest / margin;
        set_inner_viewport(vx_ + static_cast<std::int32_t>(left_) - grow_left_, grow_left_);
        send_transform({s, s - 1 - 2 * smear * inset / margin});
        inner_.execute(c);
    }
    if (grow_right_ > 0 && edges.right) {
        const float margin = static_cast<float>(grow_right_);
        const float s = smear * guest / margin;
        set_inner_viewport(vx_ + static_cast<std::int32_t>(left_) + vw_, grow_right_);
        send_transform({s, 1 - s + 2 * smear * inset / margin});
        inner_.execute(c);
    }
    apply_viewport();
    return result;
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
            Scene scene;
            if (!position_name_.empty() && !matrix_name_.empty()) {
                GlCall attribute;
                attribute.op = GlOp::get_attrib_location;
                attribute.a[0] = c.a[0];
                attribute.text = position_name_;
                scene.position = static_cast<std::int32_t>(inner_.execute(attribute).value);
                query.text = matrix_name_;
                scene.matrix = static_cast<std::int32_t>(inner_.execute(query).value);
            }
            scenes_[c.a[0]] = scene;
            return result;
        }
        case GlOp::delete_program:
            locations_.erase(c.a[0]);
            sent_.erase(c.a[0]);
            scenes_.erase(c.a[0]);
            break;
        case GlOp::uniform_matrix_4fv:
            if (const auto scene = scenes_.find(program_);
                scene != scenes_.end() && scene->second.matrix >= 0 && c.a[0] == static_cast<std::uint32_t>(scene->second.matrix) &&
                c.a[2] == 0 && c.data.size() >= 64) {
                std::memcpy(scene->second.clip.data(), c.data.data(), 64);
                scene->second.has_matrix = true;
            }
            break;
        case GlOp::use_program: program_ = c.a[0]; break;
        case GlOp::bind_buffer:
            if (c.a[0] == 0x8892) array_buffer_ = c.a[1];  // GL_ARRAY_BUFFER
            break;
        case GlOp::buffer_data:
            if (c.a[0] == 0x8892 && array_buffer_ != 0) {
                if (c.data.size() <= 16384) buffers_[array_buffer_] = c.data;
                else buffers_.erase(array_buffer_);
            }
            break;
        case GlOp::delete_buffer: buffers_.erase(c.id); break;
        case GlOp::vertex_attrib_pointer:
            pointers_[c.a[0]] = {c.a[1], c.a[2], c.a[4], c.a[5], c.a[6], c.a[3] != 0};
            break;
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
        case GlOp::draw_elements: return draw(c);
        default: break;
    }
    return inner_.execute(c);
}
}
