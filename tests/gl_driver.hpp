// SPDX-License-Identifier: GPL-2.0-or-later
// Helpers that issue decoded GL calls (GlCall) straight to a GlBackend, as the GLES2 import layer would.
#pragma once
#include "atlan/gles.hpp"
#include <array>
#include <cstring>
#include <string>
#include <vector>

namespace gltest {
using namespace atlan;
template <typename T> std::vector<std::byte> bytes_of(const std::vector<T>& v) {
    std::vector<std::byte> out(v.size() * sizeof(T));
    std::memcpy(out.data(), v.data(), out.size());
    return out;
}
struct Driver {
    GlBackend& gl;
    std::uint32_t next = 1;
    GlResult op(GlOp o, std::array<std::uint32_t, 8> a = {}) { GlCall c; c.op = o; c.a = a; return gl.execute(c); }
    std::uint32_t gen(GlOp o) { GlCall c; c.op = o; c.id = next++; gl.execute(c); return c.id; }
    std::uint32_t shader(std::uint32_t type, const std::string& source) {
        GlCall c; c.op = GlOp::create_shader; c.a[0] = type; c.id = next++; gl.execute(c);
        GlCall s; s.op = GlOp::shader_source; s.a[0] = c.id; s.text = source; gl.execute(s);
        op(GlOp::compile_shader, {c.id});
        return c.id;
    }
    std::uint32_t program(const std::string& vs, const std::string& fs) {
        const auto v = shader(0x8B31, vs), f = shader(0x8B30, fs);
        const auto p = gen(GlOp::create_program);
        op(GlOp::attach_shader, {p, v});
        op(GlOp::attach_shader, {p, f});
        op(GlOp::link_program, {p});
        return p;
    }
    bool linked(std::uint32_t p) { return op(GlOp::get_program_iv, {p, 0x8B82}).value == 1; }
    std::uint32_t uniform(std::uint32_t p, const char* name) { GlCall c; c.op = GlOp::get_uniform_location; c.a[0] = p; c.text = name; return gl.execute(c).value; }
    std::uint32_t attrib(std::uint32_t p, const char* name) { GlCall c; c.op = GlOp::get_attrib_location; c.a[0] = p; c.text = name; return gl.execute(c).value; }
    void uniform4(std::uint32_t loc, float x, float y, float z, float w) {
        GlCall c; c.op = GlOp::uniform_4fv; c.a = {loc, 1}; c.data = bytes_of<float>({x, y, z, w}); gl.execute(c);
    }
    void uniform1i(std::uint32_t loc, std::int32_t v) { GlCall c; c.op = GlOp::uniform_1i; c.a = {loc, std::uint32_t(v)}; gl.execute(c); }
    void matrix4(std::uint32_t loc, const std::vector<float>& m) { GlCall c; c.op = GlOp::uniform_matrix_4fv; c.a = {loc, 1, 0}; c.data = bytes_of(m); gl.execute(c); }
    void clear_color(float r, float g, float b, float a) { GlCall c; c.op = GlOp::clear_color; c.f = {r, g, b, a}; gl.execute(c); }
    // Client-side attribute `index` with `size` floats per vertex, then a draw of `count` vertices.
    void draw(std::uint32_t mode, const std::vector<std::pair<std::uint32_t, std::pair<std::uint32_t, std::vector<float>>>>& attribs, std::uint32_t count) {
        GlCall c; c.op = GlOp::draw_arrays; c.a = {mode, 0, count}; c.vertex_count = count;
        for (const auto& [index, data] : attribs) {
            op(GlOp::enable_vertex_attrib_array, {index});
            op(GlOp::vertex_attrib_pointer, {index, data.first, 0x1406, 0, 0, 0x1000, 0});
            GlAttribData a; a.index = index; a.size = data.first; a.type = 0x1406; a.bytes = bytes_of(data.second);
            c.attribs.push_back(std::move(a));
        }
        gl.execute(c);
        for (const auto& [index, data] : attribs) { (void)data; op(GlOp::disable_vertex_attrib_array, {index}); }
    }
};
inline std::vector<float> identity() { return {1,0,0,0, 0,1,0,0, 0,0,1,0, 0,0,0,1}; }
}
