// SPDX-License-Identifier: GPL-2.0-or-later
#include "atlan/gl_context.hpp"
#include <algorithm>
#include <bit>
#include <cstring>

namespace atlan {
namespace {
constexpr std::uint32_t gl_invalid_enum = 0x0500, gl_invalid_value = 0x0501, gl_invalid_operation = 0x0502;
constexpr std::uint32_t vertex_shader = 0x8B31, fragment_shader = 0x8B30;
constexpr std::uint32_t ubyte = 0x1401, ushort = 0x1403, uint_ = 0x1405;

std::uint32_t type_size(std::uint32_t type) {
    switch (type) {
        case 0x1400: case ubyte: return 1;
        case 0x1402: case ushort: return 2;
        case 0x1404: case uint_: case 0x1406: return 4;
    }
    return 0;
}

// One vertex attribute component as a float: normalized integers map to [0, 1] / [-1, 1], others convert by value.
float read_component(const std::byte* p, std::uint32_t type, bool normalized) {
    switch (type) {
        case 0x1400: { std::int8_t v; std::memcpy(&v, p, 1); return normalized ? std::max(float(v) / 127.0F, -1.0F) : float(v); }
        case ubyte: { std::uint8_t v; std::memcpy(&v, p, 1); return normalized ? float(v) / 255.0F : float(v); }
        case 0x1402: { std::int16_t v; std::memcpy(&v, p, 2); return normalized ? std::max(float(v) / 32767.0F, -1.0F) : float(v); }
        case ushort: { std::uint16_t v; std::memcpy(&v, p, 2); return normalized ? float(v) / 65535.0F : float(v); }
        case 0x1404: { std::int32_t v; std::memcpy(&v, p, 4); return float(v); }
        case uint_: { std::uint32_t v; std::memcpy(&v, p, 4); return float(v); }
        case 0x1406: { float v; std::memcpy(&v, p, 4); return v; }
    }
    return 0;
}

std::uint32_t read_index(const std::byte* p, std::uint32_t type) {
    switch (type) {
        case ubyte: return std::uint32_t(std::uint8_t(*p));
        case ushort: { std::uint16_t v; std::memcpy(&v, p, 2); return v; }
        default: { std::uint32_t v; std::memcpy(&v, p, 4); return v; }
    }
}

std::uint8_t expand(std::uint32_t value, unsigned bits) {  // replicate the high bits into the low ones
    const std::uint32_t max = (1U << bits) - 1;
    return std::uint8_t((value * 255U + max / 2) / max);
}

// GL pixel data -> RGBA8. Rows are padded to `alignment`; missing source bytes read as zero.
std::vector<std::uint8_t> to_rgba(const std::vector<std::byte>& src, std::uint32_t w, std::uint32_t h, std::uint32_t format,
                                  std::uint32_t type, std::uint32_t alignment, bool& ok) {
    ok = true;
    std::vector<std::uint8_t> out(std::size_t{w} * h * 4, 0);
    std::uint32_t channels = 0;
    switch (format) { case 0x1908: channels = 4; break; case 0x1907: channels = 3; break; case 0x190A: channels = 2; break;
                      case 0x1906: case 0x1909: channels = 1; break; default: break; }
    const bool packed = type == 0x8363 || type == 0x8033 || type == 0x8034;
    if (channels == 0 || (type != ubyte && !packed)) { ok = false; return out; }
    if (packed && ((type == 0x8363 && format != 0x1907) || (type != 0x8363 && format != 0x1908))) { ok = false; return out; }
    if (src.empty()) return out;
    const std::size_t pixel = packed ? 2 : channels;
    const std::size_t row = pixel * w;
    const std::size_t stride = (row + alignment - 1) / alignment * alignment;
    const auto byte_at = [&](std::size_t i) -> std::uint32_t { return i < src.size() ? std::uint32_t(std::uint8_t(src[i])) : 0U; };
    for (std::uint32_t y = 0; y < h; ++y) {
        for (std::uint32_t x = 0; x < w; ++x) {
            const std::size_t at = y * stride + x * pixel;
            std::uint8_t* d = &out[(std::size_t{y} * w + x) * 4];
            if (packed) {
                const std::uint32_t v = byte_at(at) | (byte_at(at + 1) << 8);
                if (type == 0x8363) { d[0] = expand(v >> 11, 5); d[1] = expand((v >> 5) & 63, 6); d[2] = expand(v & 31, 5); d[3] = 255; }
                else if (type == 0x8033) { d[0] = expand(v >> 12, 4); d[1] = expand((v >> 8) & 15, 4); d[2] = expand((v >> 4) & 15, 4); d[3] = expand(v & 15, 4); }
                else { d[0] = expand(v >> 11, 5); d[1] = expand((v >> 6) & 31, 5); d[2] = expand((v >> 1) & 31, 5); d[3] = (v & 1) ? 255 : 0; }
                continue;
            }
            switch (format) {
                case 0x1908: d[0] = byte_at(at); d[1] = byte_at(at + 1); d[2] = byte_at(at + 2); d[3] = byte_at(at + 3); break;
                case 0x1907: d[0] = byte_at(at); d[1] = byte_at(at + 1); d[2] = byte_at(at + 2); d[3] = 255; break;
                case 0x190A: d[0] = d[1] = d[2] = byte_at(at); d[3] = byte_at(at + 1); break;
                case 0x1909: d[0] = d[1] = d[2] = byte_at(at); d[3] = 255; break;
                default: d[0] = d[1] = d[2] = 0; d[3] = byte_at(at); break;  // GL_ALPHA
            }
        }
    }
    return out;
}
}

GlContext::Texture* GlContext::bound_texture(std::uint32_t unit) {
    if (unit >= unit_texture_.size() || unit_texture_[unit] == 0) return nullptr;
    const auto it = textures_.find(unit_texture_[unit]);
    return it == textures_.end() ? nullptr : &it->second;
}

void GlContext::compile_shader(std::uint32_t id) {
    const auto it = shaders_.find(id);
    if (it == shaders_.end()) { fail(gl_invalid_value); return; }
    auto& s = it->second;
    const auto stage = s.type == vertex_shader ? glsl::Stage::vertex : glsl::Stage::fragment;
    s.unit = std::make_shared<glsl::ShaderUnit>(glsl::compile(stage, s.source));
    s.compiled = s.unit->ok;
    s.log = s.unit->log;
    if (!s.compiled) {
        ++stats_.shader_failures;
        note(std::string(s.type == vertex_shader ? "vertex" : "fragment") + " shader " + std::to_string(id) + " failed: " + s.log);
    }
}

void GlContext::link_program(std::uint32_t id) {
    const auto it = programs_.find(id);
    if (it == programs_.end()) { fail(gl_invalid_value); return; }
    auto& p = it->second;
    const Shader *vs = nullptr, *fs = nullptr;
    for (const auto sid : p.shaders) {
        const auto s = shaders_.find(sid);
        if (s == shaders_.end()) continue;
        (s->second.type == vertex_shader ? vs : fs) = &s->second;
    }
    if (p.info) sink_.forget_program(p.info->serial);
    p.info.reset();
    if (!vs || !fs || !vs->compiled || !fs->compiled) {
        p.log = "ERROR: a program needs one successfully compiled vertex and one fragment shader\n";
        ++stats_.link_failures;
        note("program " + std::to_string(id) + " link failed: " + p.log);
        return;
    }
    auto linked = glsl::link(*vs->unit, *fs->unit);
    p.log = linked.log;
    if (!linked.ok) {
        ++stats_.link_failures;
        note("program " + std::to_string(id) + " link failed: " + p.log);
        return;
    }
    if (!p.log.empty()) note("program " + std::to_string(id) + ": " + p.log);
    p.info = std::make_shared<GlProgramInfo>(GlProgramInfo{next_serial_++, std::move(linked)});
    p.uniforms.assign(p.info->linked.uniform_bytes, std::byte{0});
    p.sampler_units.assign(p.info->linked.samplers.size(), 0);
    ++stats_.programs_linked;
}

void GlContext::set_uniform(const GlCall& c) {
    if (current_program_ == 0) { fail(gl_invalid_operation); return; }
    const auto it = programs_.find(current_program_);
    if (it == programs_.end() || !it->second.info) { fail(gl_invalid_operation); return; }
    auto& p = it->second;
    const auto& linked = p.info->linked;
    const std::uint32_t loc = c.a[0];
    if (loc == ~0u) return;  // glUniform on location -1 is silently ignored
    if (loc >= linked.sampler_location_base) {
        const auto index = loc - linked.sampler_location_base;
        if (index >= p.sampler_units.size() || c.op != GlOp::uniform_1i) { fail(gl_invalid_operation); return; }
        p.sampler_units[index] = static_cast<std::int32_t>(c.a[1]);
        return;
    }
    const glsl::UniformSlot* slot = nullptr;
    for (const auto& u : linked.uniforms) if (loc >= u.location && loc < u.location + u.count()) slot = &u;
    if (!slot) { fail(gl_invalid_operation); return; }
    const std::uint32_t element = loc - slot->location;
    const auto base = slot->type.element();
    std::byte* dst = p.uniforms.data() + slot->offset + element * slot->stride;
    const std::uint32_t room = slot->count() - element;
    const auto store = [&](std::uint32_t count, const void* src, std::uint32_t bytes_each) {
        const auto n = std::min(count, room);
        for (std::uint32_t i = 0; i < n; ++i) std::memcpy(dst + i * slot->stride, static_cast<const std::byte*>(src) + i * bytes_each, bytes_each);
    };
    switch (c.op) {
        case GlOp::uniform_1f:
            if (base.base != glsl::Base::float_ || !base.is_scalar()) { fail(gl_invalid_operation); return; }
            store(1, &c.f[0], 4);
            return;
        case GlOp::uniform_1i: {
            if (!(base.base == glsl::Base::int_ || base.base == glsl::Base::bool_) || !base.is_scalar()) { fail(gl_invalid_operation); return; }
            const std::int32_t v = static_cast<std::int32_t>(c.a[1]);
            store(1, &v, 4);
            return;
        }
        case GlOp::uniform_2fv:
        case GlOp::uniform_4fv: {
            const unsigned lanes = c.op == GlOp::uniform_2fv ? 2 : 4;
            if (base.base != glsl::Base::float_ || !base.is_vector() || base.rows != lanes) { fail(gl_invalid_operation); return; }
            const auto count = std::min<std::uint32_t>(c.a[1], static_cast<std::uint32_t>(c.data.size() / (lanes * 4)));
            store(count, c.data.data(), lanes * 4);
            return;
        }
        case GlOp::uniform_matrix_4fv: {
            if (base.base != glsl::Base::float_ || !base.is_matrix() || base.cols != 4) { fail(gl_invalid_operation); return; }
            if (c.a[2] != 0) { fail(gl_invalid_value); return; }  // GLES2 forbids transpose
            const auto count = std::min<std::uint32_t>(c.a[1], static_cast<std::uint32_t>(c.data.size() / 64));
            store(count, c.data.data(), 64);
            return;
        }
        default: return;
    }
}

void GlContext::refresh(Texture& t) {
    if (!t.dirty) return;
    t.dirty = false;
    if (t.image) sink_.forget_texture(t.image->serial);
    t.image.reset();
    if (t.levels.empty() || t.levels[0].width == 0) return;
    auto image = std::make_shared<GlTextureImage>();
    image->width = t.levels[0].width;
    image->height = t.levels[0].height;
    image->rgba = t.levels[0].rgba;
    // A mip chain counts only when every level down to 1x1 is present with the sizes GL derives from level 0.
    std::uint32_t w = image->width, h = image->height;
    std::vector<GlTextureLevel> chain;
    bool complete = true;
    for (std::size_t l = 1; (w > 1 || h > 1) && complete; ++l) {
        w = std::max(1U, w / 2);
        h = std::max(1U, h / 2);
        if (l < t.levels.size() && t.levels[l].width == w && t.levels[l].height == h) chain.push_back(t.levels[l]);
        else complete = false;
    }
    if (complete) image->mips = std::move(chain);
    image->serial = next_serial_++;
    t.image = std::move(image);
}

void GlContext::tex_image(const GlCall& c) {
    // a = target level internalformat width height border format type
    if (c.a[0] != 0x0DE1) { fail(gl_invalid_enum); return; }
    const auto id = unit_texture_[active_unit_];
    const auto it = textures_.find(id);
    if (id == 0 || it == textures_.end()) { fail(gl_invalid_operation); return; }
    const std::uint32_t level = c.a[1], w = c.a[3], h = c.a[4];
    if (level > 15 || w == 0 || h == 0 || w > 16384 || h > 16384) { fail(gl_invalid_value); return; }
    bool ok = true;
    GlTextureLevel image;
    image.width = w;
    image.height = h;
    image.rgba = to_rgba(c.data, w, h, c.a[6], c.a[7], unpack_alignment_, ok);
    if (!ok) { ++stats_.unsupported; fail(gl_invalid_enum); note("glTexImage2D format/type 0x" + std::to_string(c.a[6]) + "/0x" + std::to_string(c.a[7]) + " is not supported"); return; }
    auto& t = it->second;
    if (t.levels.size() <= level) t.levels.resize(level + 1);
    t.levels[level] = std::move(image);
    t.dirty = true;
    ++stats_.texture_uploads;
}

void GlContext::draw(const GlCall& c) {
    const auto pit = programs_.find(current_program_);
    if (current_program_ == 0 || pit == programs_.end() || !pit->second.info) {
        ++stats_.draws_dropped;
        fail(gl_invalid_operation);
        note("draw dropped: no linked program is in use");
        return;
    }
    auto& program = pit->second;
    const auto& linked = program.info->linked;
    // One packet reused for every draw, so its vectors keep their capacity instead of being allocated per draw.
    GlDraw& draw = scratch_;
    const bool elements = c.op == GlOp::draw_elements;
    const std::uint32_t mode = c.a[0];

    // indices
    auto& indices = draw.indices;
    indices.clear();
    std::uint32_t vertex_count = 0;
    if (elements) {
        const std::uint32_t count = c.a[1], type = c.a[2];
        const auto size = type_size(type);
        const std::byte* src = nullptr;
        std::size_t avail = 0;
        if (c.a[4] != 0) {
            const auto b = buffers_.find(c.a[4]);
            if (b != buffers_.end() && c.a[3] <= b->second.data.size()) { src = b->second.data.data() + c.a[3]; avail = b->second.data.size() - c.a[3]; }
        } else {
            src = c.data.data();
            avail = c.data.size();
        }
        if (!src || size == 0 || avail < std::size_t{count} * size) { ++stats_.draws_dropped; fail(gl_invalid_operation); note("glDrawElements dropped: index data is missing"); return; }
        indices.resize(count);
        for (std::uint32_t i = 0; i < count; ++i) {
            indices[i] = read_index(src + std::size_t{i} * size, type);
            vertex_count = std::max(vertex_count, indices[i] + 1);
        }
    } else {
        const std::uint32_t first = c.a[1], count = c.a[2];
        indices.resize(count);
        for (std::uint32_t i = 0; i < count; ++i) indices[i] = first + i;
        vertex_count = first + count;
    }
    if (indices.empty() || vertex_count > (1U << 24)) return;
    switch (mode) {
        case 0: draw.primitive = GlPrimitive::points; break;
        case 1: draw.primitive = GlPrimitive::lines; break;
        case 2: draw.primitive = GlPrimitive::line_strip; indices.push_back(indices.front()); break;
        case 3: draw.primitive = GlPrimitive::line_strip; break;
        case 4: draw.primitive = GlPrimitive::triangles; break;
        case 5: draw.primitive = GlPrimitive::triangle_strip; break;
        case 6:
            draw.primitive = GlPrimitive::triangles;
            fan_.assign(indices.begin(), indices.end());
            indices.clear();
            for (std::size_t i = 1; i + 1 < fan_.size(); ++i) { indices.push_back(fan_[0]); indices.push_back(fan_[i]); indices.push_back(fan_[i + 1]); }
            break;
        default: fail(gl_invalid_enum); return;
    }
    draw.vertex_count = vertex_count;

    // vertex streams, converted to float and widened to the shader attribute's size
    draw.streams.resize(linked.attributes.size());
    for (std::size_t k = 0; k < linked.attributes.size(); ++k) {
        auto& stream = draw.streams[k];
        const std::uint32_t lanes = linked.attributes[k].type.rows;
        stream.components = lanes;
        stream.values.assign(std::size_t{vertex_count} * lanes, 0.0F);
        for (std::uint32_t v = 0; v < vertex_count; ++v) if (lanes == 4) stream.values[std::size_t{v} * 4 + 3] = 1.0F;
        if (k >= attributes_.size() || !attributes_[k].enabled) continue;
        const auto& a = attributes_[k];
        const auto element = type_size(a.type);
        if (element == 0 || a.size == 0 || a.size > 4) continue;
        const std::uint32_t stride = a.stride ? a.stride : a.size * element;
        const std::byte* base = nullptr;
        std::size_t avail = 0;
        if (a.buffer != 0) {
            const auto b = buffers_.find(a.buffer);
            if (b != buffers_.end() && a.pointer <= b->second.data.size()) { base = b->second.data.data() + a.pointer; avail = b->second.data.size() - a.pointer; }
        } else {
            for (const auto& d : c.attribs) if (d.index == k) { base = d.bytes.data(); avail = d.bytes.size(); }
        }
        if (!base) continue;
        for (std::uint32_t v = 0; v < vertex_count; ++v) {
            const std::size_t at = std::size_t{v} * stride;
            if (at + std::size_t{a.size} * element > avail) break;
            for (std::uint32_t j = 0; j < std::min(a.size, lanes); ++j)
                stream.values[std::size_t{v} * lanes + j] = read_component(base + at + j * element, a.type, a.normalized);
        }
    }

    // textures
    draw.textures.assign(linked.samplers.size(), GlDraw::Binding{});
    for (std::size_t s = 0; s < linked.samplers.size(); ++s) {
        const auto unit = static_cast<std::uint32_t>(program.sampler_units[s]);
        if (auto* t = bound_texture(unit)) {
            refresh(*t);
            const bool mipmapped = t->sampler.min_filter >= 0x2700 && t->sampler.min_filter <= 0x2703;
            // GL: a texture sampled with a mipmapped minification filter and no complete chain is incomplete and reads black.
            const bool incomplete = mipmapped && t->image && t->image->mips.empty() && (t->image->width > 1 || t->image->height > 1);
            if (!incomplete) draw.textures[s].image = t->image;
            draw.textures[s].sampler = t->sampler;
        }
    }
    draw.program = program.info;
    draw.uniforms = program.uniforms;
    draw.state = state_;
    sink_.draw(draw);
    ++stats_.draws;
    // The packet keeps its buffers, not its references: deleted programs and textures are released as before.
    draw.program.reset();
    for (auto& binding : draw.textures) binding.image.reset();
}

GlResult GlContext::query_shader(const GlCall& c) const {
    GlResult r;
    const auto it = shaders_.find(c.a[0]);
    if (it == shaders_.end()) return r;
    switch (c.a[1]) {
        case 0x8B81: r.value = it->second.compiled ? 1 : 0; break;                         // COMPILE_STATUS
        case 0x8B84: r.value = it->second.log.empty() ? 0 : std::uint32_t(it->second.log.size() + 1); break;  // INFO_LOG_LENGTH
        case 0x8B4F: r.value = it->second.type; break;                                      // SHADER_TYPE
        case 0x8B88: r.value = std::uint32_t(it->second.source.size() + 1); break;          // SHADER_SOURCE_LENGTH
        default: break;
    }
    return r;
}

GlResult GlContext::query_program(const GlCall& c) const {
    GlResult r;
    const auto it = programs_.find(c.a[0]);
    if (it == programs_.end()) return r;
    const auto& p = it->second;
    switch (c.a[1]) {
        case 0x8B82: r.value = p.info ? 1 : 0; break;                                       // LINK_STATUS
        case 0x8B83: r.value = p.info ? 1 : 0; break;                                       // VALIDATE_STATUS
        case 0x8B84: r.value = p.log.empty() ? 0 : std::uint32_t(p.log.size() + 1); break;  // INFO_LOG_LENGTH
        case 0x8B85: r.value = std::uint32_t(p.shaders.size()); break;                      // ATTACHED_SHADERS
        case 0x8B86: if (p.info) r.value = std::uint32_t(p.info->linked.uniforms.size() + p.info->linked.samplers.size()); break;  // ACTIVE_UNIFORMS
        case 0x8B89: if (p.info) r.value = std::uint32_t(p.info->linked.attributes.size()); break;  // ACTIVE_ATTRIBUTES
        default: break;
    }
    return r;
}

GlResult GlContext::execute(const GlCall& c) {
    GlResult r;
    switch (c.op) {
        case GlOp::create_shader:
            if (c.a[0] != vertex_shader && c.a[0] != fragment_shader) { fail(gl_invalid_enum); break; }
            shaders_[c.id].type = c.a[0];
            break;
        case GlOp::shader_source: if (const auto it = shaders_.find(c.a[0]); it != shaders_.end()) it->second.source = c.text; else fail(gl_invalid_value); break;
        case GlOp::compile_shader: compile_shader(c.a[0]); break;
        case GlOp::delete_shader: shaders_.erase(c.a[0]); break;
        case GlOp::get_shader_iv: r = query_shader(c); break;
        case GlOp::get_shader_info_log: if (const auto it = shaders_.find(c.a[0]); it != shaders_.end()) r.text = it->second.log; break;
        case GlOp::create_program: programs_[c.id]; break;
        case GlOp::attach_shader: if (const auto it = programs_.find(c.a[0]); it != programs_.end()) it->second.shaders.push_back(c.a[1]); else fail(gl_invalid_value); break;
        case GlOp::link_program: link_program(c.a[0]); break;
        case GlOp::use_program:
            if (c.a[0] != 0 && !programs_.contains(c.a[0])) { fail(gl_invalid_value); break; }
            current_program_ = c.a[0];
            break;
        case GlOp::delete_program:
            if (const auto it = programs_.find(c.a[0]); it != programs_.end()) {
                if (it->second.info) sink_.forget_program(it->second.info->serial);
                programs_.erase(it);
            }
            if (current_program_ == c.a[0]) current_program_ = 0;
            break;
        case GlOp::get_program_iv: r = query_program(c); break;
        case GlOp::get_attrib_location:
        case GlOp::get_uniform_location: {
            const auto it = programs_.find(c.a[0]);
            if (it == programs_.end() || !it->second.info) { fail(gl_invalid_operation); r.value = ~0u; break; }
            const auto& linked = it->second.info->linked;
            r.value = c.op == GlOp::get_attrib_location ? linked.attribute_location(c.text) : linked.uniform_location(c.text);
            break;
        }
        case GlOp::uniform_1f: case GlOp::uniform_1i: case GlOp::uniform_2fv: case GlOp::uniform_4fv: case GlOp::uniform_matrix_4fv:
            set_uniform(c);
            break;
        case GlOp::gen_buffer: buffers_[c.id]; break;
        case GlOp::delete_buffer:
            buffers_.erase(c.id);
            if (array_buffer_ == c.id) array_buffer_ = 0;
            if (element_buffer_ == c.id) element_buffer_ = 0;
            for (auto& a : attributes_) if (a.buffer == c.id) a.buffer = 0;
            break;
        case GlOp::bind_buffer:
            if (c.a[0] == 0x8892) array_buffer_ = c.a[1];
            else if (c.a[0] == 0x8893) element_buffer_ = c.a[1];
            else { fail(gl_invalid_enum); break; }
            if (c.a[1] != 0) buffers_[c.a[1]];
            break;
        case GlOp::buffer_data: {
            const auto id = c.a[0] == 0x8892 ? array_buffer_ : c.a[0] == 0x8893 ? element_buffer_ : 0;
            if (id == 0) { fail(gl_invalid_operation); break; }
            auto& b = buffers_[id];
            b.data = c.data;
            b.data.resize(c.a[1], std::byte{0});
            break;
        }
        case GlOp::gen_texture: textures_[c.id]; break;
        case GlOp::delete_texture:
            if (const auto it = textures_.find(c.id); it != textures_.end()) {
                if (it->second.image) sink_.forget_texture(it->second.image->serial);
                textures_.erase(it);
            }
            for (auto& t : unit_texture_) if (t == c.id) t = 0;
            break;
        case GlOp::is_texture: { const auto it = textures_.find(c.a[0]); r.value = it != textures_.end() && it->second.bound_once ? 1 : 0; break; }
        case GlOp::active_texture:
            if (c.a[0] < 0x84C0 || c.a[0] >= 0x84C0 + unit_texture_.size()) { fail(gl_invalid_enum); break; }
            active_unit_ = c.a[0] - 0x84C0;
            break;
        case GlOp::bind_texture:
            if (c.a[0] != 0x0DE1) { fail(gl_invalid_enum); break; }
            if (c.a[1] != 0) textures_[c.a[1]].bound_once = true;
            unit_texture_[active_unit_] = c.a[1];
            break;
        case GlOp::pixel_store_i:
            if (c.a[0] == 0x0CF5 && (c.a[1] == 1 || c.a[1] == 2 || c.a[1] == 4 || c.a[1] == 8)) unpack_alignment_ = c.a[1];
            break;
        case GlOp::tex_image_2d: tex_image(c); break;
        case GlOp::compressed_tex_image_2d:
            ++stats_.unsupported;
            fail(gl_invalid_enum);
            note("glCompressedTexImage2D format 0x" + std::to_string(c.a[2]) + " is not supported");
            break;
        case GlOp::tex_parameter_i:
        case GlOp::tex_parameter_f: {
            if (c.a[0] != 0x0DE1) { fail(gl_invalid_enum); break; }
            const auto id = unit_texture_[active_unit_];
            const auto it = textures_.find(id);
            if (id == 0 || it == textures_.end()) { fail(gl_invalid_operation); break; }
            const std::uint32_t value = c.op == GlOp::tex_parameter_f ? static_cast<std::uint32_t>(c.f[0]) : c.a[2];
            auto& s = it->second.sampler;
            switch (c.a[1]) {
                case 0x2800: s.mag_filter = value; break;
                case 0x2801: s.min_filter = value; break;
                case 0x2802: s.wrap_s = value; break;
                case 0x2803: s.wrap_t = value; break;
                default: break;
            }
            break;
        }
        case GlOp::enable:
        case GlOp::disable: {
            const bool on = c.op == GlOp::enable;
            switch (c.a[0]) {
                case 0x0BE2: state_.blend = on; break;
                case 0x0B44: state_.cull = on; break;
                case 0x0B71: state_.depth_test = on; break;
                case 0x0BD0: case 0x0B90: case 0x0C11: case 0x8037: case 0x809E: case 0x809F: break;  // dither, stencil, scissor, offset, coverage: no effect
                default: fail(gl_invalid_enum); break;
            }
            break;
        }
        case GlOp::blend_func: state_.blend_src = c.a[0]; state_.blend_dst = c.a[1]; break;
        case GlOp::cull_face: state_.cull_face = c.a[0]; break;
        case GlOp::front_face: state_.front_face = c.a[0]; break;
        case GlOp::depth_func: state_.depth_func = c.a[0]; break;
        case GlOp::depth_mask: state_.depth_write = c.a[0] != 0; break;
        case GlOp::viewport:
            for (int i = 0; i < 4; ++i) state_.viewport[i] = static_cast<std::int32_t>(c.a[i]);
            break;
        case GlOp::clear_color: for (int i = 0; i < 4; ++i) clear_.rgba[i] = c.f[i]; break;
        case GlOp::clear_depthf: clear_.depth_value = c.f[0]; break;
        case GlOp::clear: {
            GlClear clear = clear_;
            clear.color = (c.a[0] & 0x4000) != 0;
            clear.depth = (c.a[0] & 0x0100) != 0 && state_.depth_write;
            if (clear.color || clear.depth) { sink_.clear(clear); ++stats_.clears; }
            break;
        }
        case GlOp::enable_vertex_attrib_array:
        case GlOp::disable_vertex_attrib_array:
            if (c.a[0] < attributes_.size()) attributes_[c.a[0]].enabled = c.op == GlOp::enable_vertex_attrib_array;
            break;
        case GlOp::vertex_attrib_pointer:
            if (c.a[0] < attributes_.size()) {
                auto& a = attributes_[c.a[0]];
                a.size = c.a[1]; a.type = c.a[2]; a.normalized = c.a[3] != 0; a.stride = c.a[4]; a.pointer = c.a[5]; a.buffer = c.a[6];
            }
            break;
        case GlOp::draw_arrays:
        case GlOp::draw_elements: draw(c); break;
        case GlOp::swap_buffers: sink_.present(); ++stats_.frames; break;
        case GlOp::read_pixels: {
            const auto w = c.a[2], h = c.a[3];
            if (!sink_.read_pixels(static_cast<std::int32_t>(c.a[0]), static_cast<std::int32_t>(c.a[1]), w, h, r.bytes)) r.bytes.assign(std::size_t{w} * h * 4, std::byte{0});
            break;
        }
        case GlOp::get_error: r.value = error_; error_ = 0; break;
        case GlOp::get_string:
            switch (c.a[0]) {
                case 0x1F00: r.text = "Atlan"; break;
                case 0x1F01: r.text = sink_.renderer(); break;
                case 0x1F02: r.text = "OpenGL ES 2.0 Atlan"; break;
                case 0x1F03: r.text = "GL_OES_texture_npot"; break;
                case 0x8B8C: r.text = "OpenGL ES GLSL ES 1.00"; break;
                default: fail(gl_invalid_enum); break;
            }
            break;
        case GlOp::finish: case GlOp::line_width: break;
    }
    return r;
}
}
