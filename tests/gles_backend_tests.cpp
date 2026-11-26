// SPDX-License-Identifier: GPL-2.0-or-later
// GlesBackend against a real GLES2 (Mesa's surfaceless EGL on Linux): guest-style calls with service-allocated ids,
// client-side arrays, buffers, textures and read-back, checked by pixels of the presented frame.
#include "egl_host.hpp"
#include "gles_backend.hpp"
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

namespace {
using atlan::GlCall;
using atlan::GlOp;
int failures = 0;
void check(bool ok, const char* what) {
    if (!ok) {
        std::printf("FAIL %s\n", what);
        ++failures;
    }
}
template <class T>
std::vector<std::byte> bytes(const std::vector<T>& v) {
    std::vector<std::byte> out(v.size() * sizeof(T));
    std::memcpy(out.data(), v.data(), out.size());
    return out;
}
GlCall op(GlOp o, std::initializer_list<std::uint32_t> a = {}) {
    GlCall c;
    c.op = o;
    std::size_t i = 0;
    for (auto v : a) c.a[i++] = v;
    return c;
}
constexpr std::uint32_t GL_VERTEX_SHADER = 0x8B31, GL_FRAGMENT_SHADER = 0x8B30, GL_COMPILE_STATUS = 0x8B81,
                        GL_LINK_STATUS = 0x8B82, GL_FLOAT = 0x1406, GL_TRIANGLE_STRIP = 5, GL_TRIANGLES = 4,
                        GL_COLOR_BUFFER_BIT = 0x4000, GL_DEPTH_BUFFER_BIT = 0x100, GL_ARRAY_BUFFER = 0x8892,
                        GL_STATIC_DRAW = 0x88E4, GL_UNSIGNED_SHORT = 0x1403, GL_TEXTURE_2D = 0x0DE1, GL_RGBA = 0x1908,
                        GL_UNSIGNED_BYTE = 0x1401, GL_TEXTURE_MIN_FILTER = 0x2801, GL_TEXTURE_MAG_FILTER = 0x2800,
                        GL_NEAREST = 0x2600, GL_EXTENSIONS = 0x1F03;

struct Pixel { std::uint8_t r, g, b, a; };
Pixel at(const std::vector<std::uint8_t>& rgba, std::uint32_t width, std::uint32_t x, std::uint32_t y) {
    const auto* p = &rgba[(std::size_t{y} * width + x) * 4];
    return {p[0], p[1], p[2], p[3]};
}
bool near(Pixel p, int r, int g, int b) {
    auto close = [](int a, int b) { return a - b < 3 && b - a < 3; };
    return close(p.r, r) && close(p.g, g) && close(p.b, b);
}

std::uint32_t program(atlan::GlesBackend& gl, std::uint32_t& next, const char* vs, const char* fs) {
    std::uint32_t shaders[2] = {next++, next++};
    const char* sources[2] = {vs, fs};
    const std::uint32_t types[2] = {GL_VERTEX_SHADER, GL_FRAGMENT_SHADER};
    for (int i = 0; i < 2; ++i) {
        auto create = op(GlOp::create_shader, {types[i]});
        create.id = shaders[i];
        gl.execute(create);
        auto source = op(GlOp::shader_source, {shaders[i]});
        source.text = sources[i];
        gl.execute(source);
        gl.execute(op(GlOp::compile_shader, {shaders[i]}));
        const auto status = gl.execute(op(GlOp::get_shader_iv, {shaders[i], GL_COMPILE_STATUS})).value;
        if (status != 1) std::printf("shader log: %s\n", gl.execute(op(GlOp::get_shader_info_log, {shaders[i]})).text.c_str());
        check(status == 1, "shader compiles");
    }
    const std::uint32_t id = next++;
    auto create = op(GlOp::create_program);
    create.id = id;
    gl.execute(create);
    gl.execute(op(GlOp::attach_shader, {id, shaders[0]}));
    gl.execute(op(GlOp::attach_shader, {id, shaders[1]}));
    gl.execute(op(GlOp::link_program, {id}));
    check(gl.execute(op(GlOp::get_program_iv, {id, GL_LINK_STATUS})).value == 1, "program links");
    return id;
}
}

int main() {
    atlan::EglHost egl;
    std::vector<std::uint8_t> frame;
    std::uint32_t frame_width = 0, frame_height = 0;
    egl.set_capture([&](const std::vector<std::uint8_t>& rgba, std::uint32_t w, std::uint32_t h) {
        frame = rgba;
        frame_width = w;
        frame_height = h;
    });
    std::string log;
    egl.set_log([&](const std::string& line) { log += line + "\n"; });
    atlan::GlesBackend gl(egl);
    gl.set_log([&](const std::string& line) { log += line + "\n"; });
    gl.set_surface_size(64, 32);
    // The first call attaches; without a usable EGL display the backend reports it and drops calls.
    const auto extensions = gl.execute(op(GlOp::get_string, {GL_EXTENSIONS}));
    if (log.find("gles unavailable") != std::string::npos) {
        std::printf("SKIP no GLES2: %s", log.c_str());
        return 77;
    }
    std::printf("%s", log.c_str());

    std::uint32_t next = 1;
    const auto flat = program(gl, next,
        "attribute vec2 pos;\nvoid main() { gl_Position = vec4(pos, 0.0, 1.0); }\n",
        "precision mediump float;\nuniform vec4 color;\nvoid main() { gl_FragColor = color; }\n");
    auto locate = op(GlOp::get_attrib_location, {flat});
    locate.text = "pos";
    const auto pos = gl.execute(locate).value;
    auto ulocate = op(GlOp::get_uniform_location, {flat});
    ulocate.text = "color";
    const auto color = gl.execute(ulocate).value;
    check(pos == 0 || pos == 1, "attribute location");
    check(color != 0xFFFFFFFFU, "uniform location");

    // Clear blue, then a red client-array quad over the left half.
    auto clear_color = op(GlOp::clear_color);
    clear_color.f = {0, 0, 1, 1};
    gl.execute(clear_color);
    gl.execute(op(GlOp::viewport, {0, 0, 64, 32}));
    gl.execute(op(GlOp::clear, {GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT}));
    gl.execute(op(GlOp::use_program, {flat}));
    auto red = op(GlOp::uniform_4fv, {color, 1});
    red.data = bytes(std::vector<float>{1, 0, 0, 1});
    gl.execute(red);
    gl.execute(op(GlOp::enable_vertex_attrib_array, {pos}));
    gl.execute(op(GlOp::vertex_attrib_pointer, {pos, 2, GL_FLOAT, 0, 0, 0x12340000, 0}));  // a guest address
    auto draw = op(GlOp::draw_arrays, {GL_TRIANGLE_STRIP, 0, 4});
    draw.vertex_count = 4;
    draw.attribs.push_back({pos, 2, GL_FLOAT, 0, false, bytes(std::vector<float>{-1, -1, 0, -1, -1, 1, 0, 1})});
    gl.execute(draw);

    // A green triangle pair from a buffer with client-side indices over the right top quarter.
    const std::uint32_t buffer = next++;
    auto gen = op(GlOp::gen_buffer);
    gen.id = buffer;
    gl.execute(gen);
    gl.execute(op(GlOp::bind_buffer, {GL_ARRAY_BUFFER, buffer}));
    auto upload = op(GlOp::buffer_data, {GL_ARRAY_BUFFER, 32, 0, GL_STATIC_DRAW});
    upload.data = bytes(std::vector<float>{0, 0, 1, 0, 0, 1, 1, 1});
    gl.execute(upload);
    gl.execute(op(GlOp::vertex_attrib_pointer, {pos, 2, GL_FLOAT, 0, 0, 0, buffer}));
    gl.execute(op(GlOp::bind_buffer, {GL_ARRAY_BUFFER, 0}));
    auto green = op(GlOp::uniform_4fv, {color, 1});
    green.data = bytes(std::vector<float>{0, 1, 0, 1});
    gl.execute(green);
    auto elements = op(GlOp::draw_elements, {GL_TRIANGLES, 6, GL_UNSIGNED_SHORT, 0x12350000, 0});
    elements.data = bytes(std::vector<std::uint16_t>{0, 1, 2, 2, 1, 3});
    gl.execute(elements);

    // A 2x2 nearest-filtered texture over the right bottom quarter: its top-left texel (white) at the quad's
    // bottom-left.
    const auto textured = program(gl, next,
        "attribute vec2 pos;\nvarying vec2 uv;\nvoid main() { uv = vec2(pos.x, pos.y + 1.0); gl_Position = vec4(pos, 0.0, 1.0); }\n",
        "precision mediump float;\nvarying vec2 uv;\nuniform sampler2D tex;\nvoid main() { gl_FragColor = texture2D(tex, uv); }\n");
    const std::uint32_t texture = next++;
    auto gen_texture = op(GlOp::gen_texture);
    gen_texture.id = texture;
    gl.execute(gen_texture);
    gl.execute(op(GlOp::bind_texture, {GL_TEXTURE_2D, texture}));
    gl.execute(op(GlOp::tex_parameter_i, {GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST}));
    gl.execute(op(GlOp::tex_parameter_i, {GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST}));
    auto image = op(GlOp::tex_image_2d, {GL_TEXTURE_2D, 0, GL_RGBA, 2, 2, 0, GL_RGBA, GL_UNSIGNED_BYTE});
    image.data = bytes(std::vector<std::uint8_t>{255, 255, 255, 255, 255, 255, 0, 255, 255, 0, 255, 255, 0, 255, 255, 255});
    gl.execute(image);
    gl.execute(op(GlOp::use_program, {textured}));
    auto tlocate = op(GlOp::get_attrib_location, {textured});
    tlocate.text = "pos";
    const auto tpos = gl.execute(tlocate).value;
    gl.execute(op(GlOp::enable_vertex_attrib_array, {tpos}));
    gl.execute(op(GlOp::vertex_attrib_pointer, {tpos, 2, GL_FLOAT, 0, 0, 0x12360000, 0}));
    auto quad = op(GlOp::draw_arrays, {GL_TRIANGLE_STRIP, 0, 4});
    quad.vertex_count = 4;
    quad.attribs.push_back({tpos, 2, GL_FLOAT, 0, false, bytes(std::vector<float>{0.01F, -1, 1, -1, 0.01F, -0.01F, 1, -0.01F})});
    gl.execute(quad);

    const auto read = gl.execute(op(GlOp::read_pixels, {0, 0, 64, 32, GL_RGBA, GL_UNSIGNED_BYTE}));
    gl.execute(op(GlOp::swap_buffers));
    check(frame_width == 64 && frame_height == 32, "frame size");
    if (frame_width == 64 && frame_height == 32) {
        // Frame rows are top first.
        check(near(at(frame, 64, 10, 16), 255, 0, 0), "client-array quad is red");
        check(near(at(frame, 64, 48, 8), 0, 255, 0), "buffer quad is green");
        check(near(at(frame, 64, 33, 30), 255, 255, 255), "texel (0,0) at the quad's bottom-left");
        check(near(at(frame, 64, 62, 30), 255, 255, 0), "texel (1,0) at the quad's bottom-right");
        check(near(at(frame, 64, 33, 17), 255, 0, 255), "texel (0,1) at the quad's top-left");
        check(read.bytes.size() == 64 * 32 * 4, "read_pixels size");
        // read_pixels rows are bottom first: guest row 31 is the frame's top row.
        if (read.bytes.size() == 64 * 32 * 4) {
            const auto r = std::to_integer<int>(read.bytes[(31 * 64 + 10) * 4]);
            check(r == 255, "read_pixels matches the frame");
        }
    }
    // A draw whose client attribute has no gathered data is skipped, not drawn from a stale host pointer.
    auto stale = op(GlOp::draw_elements, {GL_TRIANGLES, 3, GL_UNSIGNED_SHORT, 0, 7});
    gl.execute(stale);
    check(gl.stats().draws_skipped == 1, "stale client array skipped");
    check(gl.stats().frames == 1 && gl.stats().draws == 3, "draw and frame counts");
    check(!extensions.text.empty(), "extensions string");
    if (failures == 0) std::printf("gles backend ok\n");
    return failures == 0 ? 0 : 1;
}
