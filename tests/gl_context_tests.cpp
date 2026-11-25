// SPDX-License-Identifier: GPL-2.0-or-later
#include "atlan/gl_context.hpp"
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <iostream>
#include <iterator>

using namespace atlan;
namespace {
int checks = 0;
void check(bool ok, const char* what) {
    ++checks;
    if (!ok) { std::cerr << "FAILED: " << what << '\n'; std::exit(1); }
}
std::string slurp(const std::string& path) {
    std::ifstream in(path, std::ios::binary);
    check(bool(in), ("cannot open " + path).c_str());
    return {std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>()};
}
struct Sink final : GlSink {
    std::vector<GlDraw> draws;
    std::vector<GlClear> clears;
    int presents = 0;
    std::vector<std::uint64_t> forgotten_textures, forgotten_programs;
    void clear(const GlClear& c) override { clears.push_back(c); }
    void draw(const GlDraw& d) override { draws.push_back(d); }
    void present() override { ++presents; }
    void forget_texture(std::uint64_t s) override { forgotten_textures.push_back(s); }
    void forget_program(std::uint64_t s) override { forgotten_programs.push_back(s); }
};
template <typename T> std::vector<std::byte> bytes_of(const std::vector<T>& v) {
    std::vector<std::byte> out(v.size() * sizeof(T));
    std::memcpy(out.data(), v.data(), out.size());
    return out;
}
struct Gl {
    Sink sink;
    GlContext ctx{sink};
    std::uint32_t next = 1;
    std::vector<std::string> log;
    Gl() { ctx.set_log([this](const std::string& s) { log.push_back(s); }); }
    GlResult run(GlCall c) { return ctx.execute(c); }
    GlResult op(GlOp o, std::array<std::uint32_t, 8> a = {}) { GlCall c; c.op = o; c.a = a; return ctx.execute(c); }
    std::uint32_t shader(std::uint32_t type, const std::string& source) {
        GlCall c; c.op = GlOp::create_shader; c.a[0] = type; c.id = next++; ctx.execute(c);
        GlCall s; s.op = GlOp::shader_source; s.a[0] = c.id; s.text = source; ctx.execute(s);
        op(GlOp::compile_shader, {c.id});
        return c.id;
    }
    std::uint32_t program(const std::string& vs, const std::string& fs) {
        const auto v = shader(0x8B31, vs), f = shader(0x8B30, fs);
        GlCall c; c.op = GlOp::create_program; c.id = next++; ctx.execute(c);
        op(GlOp::attach_shader, {c.id, v});
        op(GlOp::attach_shader, {c.id, f});
        op(GlOp::link_program, {c.id});
        return c.id;
    }
    std::uint32_t uniform(std::uint32_t prog, const char* name) { GlCall c; c.op = GlOp::get_uniform_location; c.a[0] = prog; c.text = name; return ctx.execute(c).value; }
    std::uint32_t attrib(std::uint32_t prog, const char* name) { GlCall c; c.op = GlOp::get_attrib_location; c.a[0] = prog; c.text = name; return ctx.execute(c).value; }
    std::uint32_t gen(GlOp o) { GlCall c; c.op = o; c.id = next++; ctx.execute(c); return c.id; }
    std::uint32_t error() { return op(GlOp::get_error).value; }
};
float f32(const std::vector<std::byte>& b, std::size_t offset) { float f; std::memcpy(&f, b.data() + offset, 4); return f; }
std::int32_t i32(const std::vector<std::byte>& b, std::size_t offset) { std::int32_t v; std::memcpy(&v, b.data() + offset, 4); return v; }
void set_pointer(Gl& gl, std::uint32_t index, std::uint32_t size, std::uint32_t type, bool normalized, std::uint32_t stride, std::uint32_t pointer, std::uint32_t buffer) {
    gl.op(GlOp::vertex_attrib_pointer, {index, size, type, normalized, stride, pointer, buffer});
}
GlCall draw_arrays(std::uint32_t mode, std::uint32_t first, std::uint32_t count) {
    GlCall c; c.op = GlOp::draw_arrays; c.a = {mode, first, count}; c.vertex_count = first + count; return c;
}
}

int main(int argc, char** argv) {
    const std::string dir = argc > 1 ? argv[1] : "tests/glsl";
    const auto flat_vs = slurp(dir + "/flat.vert"), flat_fs = slurp(dir + "/flat.frag");
    const auto scene_vs = slurp(dir + "/scene.vert"), scene_fs = slurp(dir + "/scene.frag");
    {   // compile / link / reflection
        Gl gl;
        const auto bad = gl.shader(0x8B30, "void main(){ gl_FragColor = vec4(nope); }");
        check(gl.op(GlOp::get_shader_iv, {bad, 0x8B81}).value == 0, "failed compile reports COMPILE_STATUS false");
        GlCall log; log.op = GlOp::get_shader_info_log; log.a[0] = bad;
        check(gl.ctx.execute(log).text.find("undeclared identifier 'nope'") != std::string::npos, "info log carries the compiler message");
        check(gl.op(GlOp::get_shader_iv, {bad, 0x8B84}).value > 10, "INFO_LOG_LENGTH counts the log");
        const auto prog = gl.program(flat_vs, flat_fs);
        check(gl.op(GlOp::get_program_iv, {prog, 0x8B82}).value == 1, "program links");
        check(gl.attrib(prog, "a_xy") == 0 && gl.attrib(prog, "missing") == ~0u, "attribute locations, -1 for unknown names");
        check(gl.uniform(prog, "u_color") == 0 && gl.uniform(prog, "u_enabled") == 1 && gl.uniform(prog, "nope") == ~0u, "uniform locations");
        const auto broken = gl.program("void main(){ gl_Position = ; }", "void main(){ gl_FragColor = vec4(1.0); }");
        check(gl.op(GlOp::get_program_iv, {broken, 0x8B82}).value == 0, "failed vertex shader fails the link");
        gl.op(GlOp::use_program, {broken});
        gl.run(draw_arrays(4, 0, 3));
        check(gl.sink.draws.empty() && gl.error() == 0x0502 && gl.ctx.stats().draws_dropped == 1, "draw with an unlinked program is dropped with INVALID_OPERATION");
        check(gl.error() == 0, "error flag clears when read");
    }
    {   // uniforms and client-side vertex arrays
        Gl gl;
        const auto prog = gl.program(flat_vs, flat_fs);
        gl.op(GlOp::use_program, {prog});
        GlCall u4; u4.op = GlOp::uniform_4fv; u4.a = {gl.uniform(prog, "u_color"), 1}; u4.data = bytes_of<float>({0.25F, 0.5F, 0.75F, 1.0F});
        gl.run(u4);
        GlCall u1; u1.op = GlOp::uniform_1i; u1.a = {gl.uniform(prog, "u_enabled"), 1};
        gl.run(u1);
        gl.op(GlOp::viewport, {10, 20, 300, 200});
        gl.op(GlOp::enable, {0x0BE2});
        gl.op(GlOp::blend_func, {0x0302, 0x0303});
        gl.op(GlOp::enable_vertex_attrib_array, {0});
        set_pointer(gl, 0, 2, 0x1406, false, 0, 0x1000, 0);
        auto d = draw_arrays(4, 0, 3);
        GlAttribData a; a.index = 0; a.size = 2; a.type = 0x1406;
        a.bytes = bytes_of<float>({-1, -1, 1, -1, 0, 1});
        d.attribs.push_back(a);
        gl.run(d);
        check(gl.sink.draws.size() == 1, "draw reaches the sink");
        const auto& dr = gl.sink.draws[0];
        check(dr.primitive == GlPrimitive::triangles && dr.indices == std::vector<std::uint32_t>({0, 1, 2}) && dr.vertex_count == 3, "triangle indices");
        check(dr.streams.size() == 1 && dr.streams[0].components == 2 && dr.streams[0].values == std::vector<float>({-1, -1, 1, -1, 0, 1}), "float stream");
        check(f32(dr.uniforms, 0) == 0.25F && f32(dr.uniforms, 12) == 1.0F && i32(dr.uniforms, 16) == 1, "uniform block snapshot");
        check(dr.state.viewport[2] == 300 && dr.state.blend && dr.state.blend_src == 0x0302 && dr.state.blend_dst == 0x0303, "render state snapshot");
        gl.run(u1);  // idempotent
        u1.a[1] = 0;
        gl.run(u1);
        gl.run(draw_arrays(4, 0, 3));
        check(i32(gl.sink.draws[0].uniforms, 16) == 1 && i32(gl.sink.draws[1].uniforms, 16) == 0, "snapshots are independent of later uniform changes");
        GlCall bad; bad.op = GlOp::uniform_1f; bad.a[0] = 1; bad.f[0] = 2;
        gl.run(bad);
        check(gl.error() == 0x0502, "type-mismatched uniform call sets INVALID_OPERATION");
        GlCall ignored; ignored.op = GlOp::uniform_1f; ignored.a[0] = ~0u;
        gl.run(ignored);
        check(gl.error() == 0, "uniform location -1 is silently ignored");
        // disabled attribute -> current value (0, 0, 0, 1) for a 2-wide attribute is (0, 0)
        gl.op(GlOp::disable_vertex_attrib_array, {0});
        gl.run(draw_arrays(4, 0, 3));
        check(gl.sink.draws.back().streams[0].values == std::vector<float>(6, 0.0F), "disabled attribute uses its default value");
    }
    {   // attribute conversion, widening, VBOs, primitive expansion
        Gl gl;
        const auto prog = gl.program(scene_vs, scene_fs);
        gl.op(GlOp::use_program, {prog});
        const auto pos = gl.attrib(prog, "a_pos"), uv = gl.attrib(prog, "a_uv"), extra = gl.attrib(prog, "a_extra");
        check(pos == 0 && uv == 2 && extra == 3, "scene attribute locations");
        for (auto loc : {pos, uv, extra}) gl.op(GlOp::enable_vertex_attrib_array, {loc});
        set_pointer(gl, pos, 3, 0x1406, false, 0, 0, 0);
        set_pointer(gl, uv, 2, 0x1403, true, 0, 0, 0);
        set_pointer(gl, extra, 4, 0x1401, false, 4, 0, 0);
        auto d = draw_arrays(5, 0, 4);
        GlAttribData p; p.index = pos; p.bytes = bytes_of<float>({0, 0, 0, 1, 0, 0, 0, 1, 0, 1, 1, 0});
        GlAttribData t; t.index = uv; t.bytes = bytes_of<std::uint16_t>({0, 0, 65535, 0, 0, 32768, 65535, 65535});
        GlAttribData e; e.index = extra; for (int i = 0; i < 16; ++i) e.bytes.push_back(std::byte(i));
        d.attribs = {p, t, e};
        gl.run(d);
        const auto& dr = gl.sink.draws.at(0);
        check(dr.primitive == GlPrimitive::triangle_strip, "strip stays a strip");
        check(dr.streams[pos].components == 4 && dr.streams[pos].values[3] == 1.0F && dr.streams[pos].values[4 + 0] == 1.0F, "vec3 source widened to vec4 with w = 1");
        check(dr.streams[uv].components == 2 && dr.streams[uv].values[2] == 1.0F && std::abs(dr.streams[uv].values[5] - 32768.0F / 65535.0F) < 1e-6F, "normalized ushort texture coordinates");
        check(dr.streams[extra].values[5] == 5.0F && dr.streams[extra].values[15] == 15.0F, "unnormalized ubyte converts by value");
        check(dr.streams[1].values.size() == 12 && dr.streams[1].values[2] == 0.0F, "enabled-less attribute defaults to zeros (normal)");

        // VBO + element buffer, triangle fan and line loop expansion
        const auto vbo = gl.gen(GlOp::gen_buffer), ebo = gl.gen(GlOp::gen_buffer);
        gl.op(GlOp::bind_buffer, {0x8892, vbo});
        GlCall bd; bd.op = GlOp::buffer_data; bd.a = {0x8892, 48, 0, 0x88E4};
        bd.data = bytes_of<float>({0, 0, 0, 1, 1, 0, 1, 1, 0, 0, 1, 0});
        gl.run(bd);
        gl.op(GlOp::bind_buffer, {0x8893, ebo});
        GlCall ed; ed.op = GlOp::buffer_data; ed.a = {0x8893, 8, 0, 0x88E4}; ed.data = bytes_of<std::uint16_t>({3, 2, 1, 0});
        gl.run(ed);
        set_pointer(gl, pos, 3, 0x1406, false, 0, 0, vbo);
        set_pointer(gl, uv, 2, 0x1406, false, 0, 0, vbo);
        gl.op(GlOp::disable_vertex_attrib_array, {uv});
        gl.op(GlOp::disable_vertex_attrib_array, {extra});
        GlCall fan; fan.op = GlOp::draw_elements; fan.a = {6, 4, 0x1403, 0, ebo};
        gl.run(fan);
        const auto& fd = gl.sink.draws.back();
        check(fd.indices == std::vector<std::uint32_t>({3, 2, 1, 3, 1, 0}) && fd.vertex_count == 4, "triangle fan from an element buffer expands to a list");
        check(fd.streams[pos].values[0] == 0.0F && fd.streams[pos].values[4] == 1.0F && fd.streams[pos].values[12] == 0.0F && fd.streams[pos].values[13] == 1.0F, "vertex buffer attribute");
        GlCall loop; loop.op = GlOp::draw_elements; loop.a = {2, 3, 0x1403, 2, ebo};
        gl.run(loop);
        check(gl.sink.draws.back().primitive == GlPrimitive::line_strip && gl.sink.draws.back().indices == std::vector<std::uint32_t>({2, 1, 0, 2}), "line loop closes the strip");
        GlCall oob; oob.op = GlOp::draw_elements; oob.a = {4, 100, 0x1403, 0, ebo};
        const auto before = gl.sink.draws.size();
        gl.run(oob);
        check(gl.sink.draws.size() == before && gl.ctx.stats().draws_dropped == 1, "out-of-range element draw is dropped");
    }
    {   // textures and samplers
        Gl gl;
        const auto prog = gl.program(scene_vs, scene_fs);
        gl.op(GlOp::use_program, {prog});
        const auto tex = gl.gen(GlOp::gen_texture), second = gl.gen(GlOp::gen_texture);
        gl.op(GlOp::active_texture, {0x84C1});
        gl.op(GlOp::bind_texture, {0x0DE1, tex});
        gl.op(GlOp::pixel_store_i, {0x0CF5, 1});
        GlCall img; img.op = GlOp::tex_image_2d; img.a = {0x0DE1, 0, 0x1907, 2, 1, 0, 0x1907, 0x8363};
        img.data = {std::byte(0x00), std::byte(0xF8), std::byte(0xE0), std::byte(0x07)};  // pure red, pure green in RGB565
        gl.run(img);
        GlCall filter; filter.op = GlOp::tex_parameter_i; filter.a = {0x0DE1, 0x2801, 0x2600};
        gl.run(filter);
        filter.a = {0x0DE1, 0x2802, 0x812F};
        gl.run(filter);
        GlCall mag; mag.op = GlOp::tex_parameter_f; mag.a = {0x0DE1, 0x2800}; mag.f[0] = 9728.0F;
        gl.run(mag);
        GlCall unit; unit.op = GlOp::uniform_1i; unit.a = {gl.uniform(prog, "u_tex"), 1};
        gl.run(unit);
        gl.op(GlOp::enable_vertex_attrib_array, {0});
        set_pointer(gl, 0, 2, 0x1406, false, 0, 0, 0);
        auto d = draw_arrays(4, 0, 3);
        GlAttribData a; a.index = 0; a.bytes = bytes_of<float>({0, 0, 1, 0, 0, 1});
        d.attribs.push_back(a);
        gl.run(d);
        const auto& dr = gl.sink.draws.at(0);
        check(dr.textures.size() == 1 && dr.textures[0].image, "sampler resolves to the texture bound on its unit");
        const auto& pixels = dr.textures[0].image->rgba;
        check(dr.textures[0].image->width == 2 && pixels.size() == 8 && pixels[0] == 255 && pixels[1] == 0 && pixels[3] == 255 && pixels[4] == 0 && pixels[5] == 255, "RGB565 converts to RGBA8");
        check(dr.textures[0].sampler.min_filter == 0x2600 && dr.textures[0].sampler.mag_filter == 9728 && dr.textures[0].sampler.wrap_s == 0x812F && dr.textures[0].sampler.wrap_t == 0x2901, "sampler parameters");
        gl.op(GlOp::active_texture, {0x84C0});
        unit.a[1] = 0;
        gl.run(unit);
        gl.run(d);
        check(!gl.sink.draws.back().textures[0].image, "a unit with no texture samples nothing");
        // RGBA upload with default alignment 4: width 1 RGB rows are padded to 4 bytes
        gl.op(GlOp::bind_texture, {0x0DE1, second});
        gl.op(GlOp::pixel_store_i, {0x0CF5, 4});
        GlCall rgb; rgb.op = GlOp::tex_image_2d; rgb.a = {0x0DE1, 0, 0x1907, 1, 2, 0, 0x1907, 0x1401};
        rgb.data = {std::byte(1), std::byte(2), std::byte(3), std::byte(0), std::byte(4), std::byte(5), std::byte(6)};
        gl.run(rgb);
        gl.run(unit);
        gl.run(d);
        const auto& p2 = gl.sink.draws.back().textures[0].image->rgba;
        check(p2.size() == 8 && p2[4] == 4 && p2[5] == 5 && p2[6] == 6 && p2[7] == 255, "unpack alignment pads rows");
        const auto old_serial = gl.sink.draws.back().textures[0].image->serial;
        gl.run(rgb);
        check(gl.sink.forgotten_textures.size() == 1 && gl.sink.forgotten_textures[0] == old_serial, "replacing an image tells the sink to drop the old device texture");
        GlCall odd; odd.op = GlOp::tex_image_2d; odd.a = {0x0DE1, 0, 0x1908, 1, 1, 0, 0x1908, 0x1406};
        gl.run(odd);
        check(gl.error() == 0x0500 && gl.ctx.stats().unsupported == 1, "unsupported pixel type reports INVALID_ENUM");
    }
    {   // clear, present, state
        Gl gl;
        gl.op(GlOp::clear_color, {});
        GlCall cc; cc.op = GlOp::clear_color; cc.f = {0.1F, 0.2F, 0.3F, 1.0F};
        gl.run(cc);
        gl.op(GlOp::clear, {0x4100});
        check(gl.sink.clears.size() == 1 && gl.sink.clears[0].color && gl.sink.clears[0].depth && gl.sink.clears[0].rgba[1] == 0.2F && gl.sink.clears[0].depth_value == 1.0F, "clear carries color and depth");
        gl.op(GlOp::depth_mask, {0});
        gl.op(GlOp::clear, {0x0100});
        check(gl.sink.clears.size() == 1, "depth clear is masked by glDepthMask(false)");
        gl.op(GlOp::swap_buffers);
        check(gl.sink.presents == 1 && gl.ctx.stats().frames == 1, "swap presents");
        gl.op(GlOp::enable, {0x1234});
        check(gl.error() == 0x0500, "unknown capability is INVALID_ENUM");
        GlCall s; s.op = GlOp::get_string; s.a[0] = 0x1F02;
        check(gl.ctx.execute(s).text.starts_with("OpenGL ES 2.0"), "version string");
    }
    std::cout << "gl context tests passed: " << checks << " checks\n";
}
