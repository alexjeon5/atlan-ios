// SPDX-License-Identifier: GPL-2.0-or-later
// Renders through the real Metal backend into an offscreen target and checks pixels. Skips (exit 0) without a Metal device.
// With ATLAN_SHADER_DAT=<file> it also builds a Metal pipeline for every vertex/fragment pair of a shader.dat-style
// container (count, then {length, offset} pairs) to prove the translated MSL is accepted by the Metal compiler.
#include "gl_driver.hpp"
#include "metal_gl_backend.hpp"
#include "atlan/gl_margin.hpp"
#include <cstdlib>
#include <fstream>
#include <iostream>
#include <iterator>

using namespace gltest;
namespace {
int checks = 0;
bool failed = false;
void check(bool ok, const std::string& what) {
    ++checks;
    if (!ok) { std::cerr << "FAILED: " << what << '\n'; failed = true; }
}
std::string slurp(const std::string& path) {
    std::ifstream in(path, std::ios::binary);
    return {std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>()};
}
struct Pixel { int r, g, b, a; };
constexpr std::uint32_t W = 64, H = 64;
Pixel at(const std::vector<std::uint8_t>& bgra, std::uint32_t x, std::uint32_t y) {  // y counts from the top of the image
    const auto* p = &bgra[(std::size_t{y} * W + x) * 4];
    return {p[2], p[1], p[0], p[3]};
}
bool near(Pixel p, int r, int g, int b, int tol = 2) {
    return std::abs(p.r - r) <= tol && std::abs(p.g - g) <= tol && std::abs(p.b - b) <= tol;
}
std::string show(Pixel p) { return "(" + std::to_string(p.r) + "," + std::to_string(p.g) + "," + std::to_string(p.b) + "," + std::to_string(p.a) + ")"; }

const char* flat_vs = "attribute vec3 a_pos; void main(){ gl_Position = vec4(a_pos, 1.0); }";
const char* flat_fs = "precision mediump float; uniform vec4 u_color; void main(){ gl_FragColor = u_color; }";
// a triangle that covers the whole viewport, counter-clockwise as seen on screen
const std::vector<float> cover_ccw = {-1, -1, 0, 3, -1, 0, -1, 3, 0};
const std::vector<float> cover_cw = {-1, -1, 0, -1, 3, 0, 3, -1, 0};
}

int main() {
    atlan::MetalGlBackend backend(W, H);
    if (!backend.available()) { std::cout << "metal render tests skipped: no Metal device\n"; return 0; }
    Driver gl{backend};
    std::vector<std::string> log;
    backend.set_log([&](const std::string& s) { log.push_back(s); });
    std::vector<std::uint8_t> img;
    std::uint32_t rw = 0, rh = 0;
    const auto frame = [&]() { gl.op(GlOp::swap_buffers); return backend.read_back(img, rw, rh) && rw == W && rh == H; };

    // clear + present
    gl.clear_color(0, 1, 0, 1);
    gl.op(GlOp::clear, {0x4100});
    {   // glReadPixels mid-frame: RGBA rows from the bottom, and the frame continues afterwards
        const auto pixels = gl.op(GlOp::read_pixels, {0, 0, W, H, 0x1908, 0x1401}).bytes;
        check(pixels.size() == std::size_t{W} * H * 4 && std::to_integer<int>(pixels[0]) == 0 && std::to_integer<int>(pixels[1]) == 255 &&
              std::to_integer<int>(pixels[2]) == 0 && std::to_integer<int>(pixels[3]) == 255, "glReadPixels returns the cleared color as RGBA");
        const auto outside = gl.op(GlOp::read_pixels, {W - 1, H - 1, 2, 2, 0x1908, 0x1401}).bytes;
        check(outside.size() == 16 && std::to_integer<int>(outside[1]) == 255 && std::to_integer<int>(outside[5]) == 0 && std::to_integer<int>(outside[13]) == 0,
              "glReadPixels leaves pixels outside the framebuffer zero");
    }
    check(frame() && near(at(img, 32, 32), 0, 255, 0), "clear color reaches the target");

    const auto prog = gl.program(flat_vs, flat_fs);
    check(gl.linked(prog), "flat program links");
    for (const auto& l : log) std::cerr << "log: " << l << '\n';
    gl.op(GlOp::use_program, {prog});
    const auto color = gl.uniform(prog, "u_color"), pos = gl.attrib(prog, "a_pos");
    gl.op(GlOp::viewport, {0, 0, W, H});
    const auto begin = [&](float r, float g, float b) { gl.clear_color(r, g, b, 1); gl.op(GlOp::clear, {0x4100}); };

    begin(0, 0, 0);
    gl.uniform4(color, 1, 0, 0, 1);
    gl.draw(4, {{pos, {3, cover_ccw}}}, 3);
    check(frame(), "frame 1 read back");
    check(near(at(img, 32, 32), 255, 0, 0), "a triangle with default state is visible: got " + show(at(img, 32, 32)));
    check(near(at(img, 1, 1), 255, 0, 0) && near(at(img, 62, 62), 255, 0, 0), "it covers the corners");

    // culling follows GL's winding convention (default: cull back faces, CCW is front)
    begin(0, 0, 1);
    gl.op(GlOp::enable, {0x0B44});
    gl.op(GlOp::cull_face, {0x0405});
    gl.op(GlOp::front_face, {0x0901});
    gl.draw(4, {{pos, {3, cover_ccw}}}, 3);
    check(frame() && near(at(img, 32, 32), 255, 0, 0), "CCW triangle survives back-face culling: got " + show(at(img, 32, 32)));
    begin(0, 0, 1);
    gl.draw(4, {{pos, {3, cover_cw}}}, 3);
    check(frame() && near(at(img, 32, 32), 0, 0, 255), "CW triangle is culled: got " + show(at(img, 32, 32)));
    gl.op(GlOp::front_face, {0x0900});
    begin(0, 0, 1);
    gl.draw(4, {{pos, {3, cover_cw}}}, 3);
    check(frame() && near(at(img, 32, 32), 255, 0, 0), "glFrontFace(CW) flips the facing: got " + show(at(img, 32, 32)));
    gl.op(GlOp::disable, {0x0B44});

    // viewport origin is the lower left corner
    begin(0, 0, 0);
    gl.op(GlOp::viewport, {0, 0, W, H / 2});
    gl.draw(4, {{pos, {3, cover_ccw}}}, 3);
    check(frame() && near(at(img, 32, 48), 255, 0, 0) && near(at(img, 32, 16), 0, 0, 0), "viewport (0,0,w,h/2) is the bottom half: top " + show(at(img, 32, 16)) + " bottom " + show(at(img, 32, 48)));
    gl.op(GlOp::viewport, {W / 2, 0, W / 2, H});
    begin(0, 0, 0);
    gl.op(GlOp::viewport, {W / 2, 0, W / 2, H});
    gl.draw(4, {{pos, {3, cover_ccw}}}, 3);
    check(frame() && near(at(img, 48, 32), 255, 0, 0) && near(at(img, 16, 32), 0, 0, 0), "viewport (w/2,0,w/2,h) is the right half");
    gl.op(GlOp::viewport, {0, 0, W, H});

    // blending
    begin(0, 0, 0);
    gl.op(GlOp::enable, {0x0BE2});
    gl.op(GlOp::blend_func, {0x0302, 0x0303});
    gl.uniform4(color, 1, 1, 1, 0.5F);
    gl.draw(4, {{pos, {3, cover_ccw}}}, 3);
    check(frame() && near(at(img, 32, 32), 128, 128, 128, 3), "SRC_ALPHA / ONE_MINUS_SRC_ALPHA blending: got " + show(at(img, 32, 32)));
    gl.op(GlOp::disable, {0x0BE2});

    // depth: the nearer triangle wins regardless of draw order
    const std::vector<float> far_tri = {-1, -1, 0.5F, 3, -1, 0.5F, -1, 3, 0.5F}, near_tri = {-1, -1, -0.5F, 3, -1, -0.5F, -1, 3, -0.5F};
    gl.op(GlOp::enable, {0x0B71});
    gl.op(GlOp::depth_func, {0x0201});
    for (int order = 0; order < 2; ++order) {
        begin(0, 0, 0);
        GlCall cd; cd.op = GlOp::clear_depthf; cd.f[0] = 1; backend.execute(cd);
        gl.op(GlOp::clear, {0x4100});
        gl.uniform4(color, 0, 0, 1, 1);
        if (order == 0) gl.draw(4, {{pos, {3, far_tri}}}, 3);
        gl.uniform4(color, 0, 1, 0, 1);
        gl.draw(4, {{pos, {3, near_tri}}}, 3);
        if (order == 1) { gl.uniform4(color, 0, 0, 1, 1); gl.draw(4, {{pos, {3, far_tri}}}, 3); }
        check(frame() && near(at(img, 32, 32), 0, 255, 0), std::string("depth test keeps the nearer triangle (order ") + std::to_string(order) + "): got " + show(at(img, 32, 32)));
    }
    gl.op(GlOp::disable, {0x0B71});

    // texture sampling through a translated fragment shader
    {
        const auto textured = gl.program("attribute vec2 a_xy; attribute vec2 a_uv; varying vec2 v_uv; void main(){ v_uv = a_uv; gl_Position = vec4(a_xy, 0.0, 1.0); }",
                                         "precision mediump float; uniform sampler2D u_tex; varying vec2 v_uv; void main(){ gl_FragColor = texture2D(u_tex, v_uv); }");
        check(gl.linked(textured), "textured program links");
        const auto tex = gl.gen(GlOp::gen_texture);
        gl.op(GlOp::bind_texture, {0x0DE1, tex});
        gl.op(GlOp::pixel_store_i, {0x0CF5, 1});
        GlCall img2; img2.op = GlOp::tex_image_2d; img2.a = {0x0DE1, 0, 0x1908, 2, 2, 0, 0x1908, 0x1401};
        for (unsigned char v : {255, 0, 0, 255,  0, 255, 0, 255,  0, 0, 255, 255,  255, 255, 255, 255}) img2.data.push_back(std::byte(v));
        backend.execute(img2);
        GlCall nearest; nearest.op = GlOp::tex_parameter_i; nearest.a = {0x0DE1, 0x2801, 0x2600};
        backend.execute(nearest);
        nearest.a = {0x0DE1, 0x2800, 0x2600};
        backend.execute(nearest);
        gl.op(GlOp::use_program, {textured});
        gl.uniform1i(gl.uniform(textured, "u_tex"), 0);
        begin(0, 0, 0);
        const std::vector<float> xy = {-1, -1, 1, -1, -1, 1, 1, -1, 1, 1, -1, 1}, uv = {0, 0, 1, 0, 0, 1, 1, 0, 1, 1, 0, 1};
        gl.draw(4, {{gl.attrib(textured, "a_xy"), {2, xy}}, {gl.attrib(textured, "a_uv"), {2, uv}}}, 6);
        check(frame(), "textured frame read back");
        // GL: texture row 0 is at v = 0, i.e. the bottom of this quad
        check(near(at(img, 16, 48), 255, 0, 0) && near(at(img, 48, 48), 0, 255, 0), "first texture row is at the bottom: " + show(at(img, 16, 48)) + show(at(img, 48, 48)));
        check(near(at(img, 16, 16), 0, 0, 255) && near(at(img, 48, 16), 255, 255, 255), "second texture row is at the top: " + show(at(img, 16, 16)) + show(at(img, 48, 16)));
    }

    // mip chains: an 8x8 pixel viewport showing a 64x64 texture samples level 3
    {
        const auto textured = gl.program("attribute vec2 a_xy; attribute vec2 a_uv; varying vec2 v_uv; void main(){ v_uv = a_uv; gl_Position = vec4(a_xy, 0.0, 1.0); }",
                                         "precision mediump float; uniform sampler2D u_tex; varying vec2 v_uv; void main(){ gl_FragColor = texture2D(u_tex, v_uv); }");
        const auto tex = gl.gen(GlOp::gen_texture);
        gl.op(GlOp::bind_texture, {0x0DE1, tex});
        for (std::uint32_t l = 0; l <= 6; ++l) {
            const std::uint32_t size = 64U >> l;
            GlCall c; c.op = GlOp::tex_image_2d; c.a = {0x0DE1, l, 0x1908, size, size, 0, 0x1908, 0x1401};
            for (std::uint32_t i = 0; i < size * size; ++i)
                for (unsigned char v : {(unsigned char)(l * 30), (unsigned char)0, (unsigned char)(255 - l * 30), (unsigned char)255}) c.data.push_back(std::byte(v));
            backend.execute(c);
        }
        GlCall f; f.op = GlOp::tex_parameter_i; f.a = {0x0DE1, 0x2801, 0x2700}; backend.execute(f);
        f.a = {0x0DE1, 0x2800, 0x2600}; backend.execute(f);
        gl.op(GlOp::use_program, {textured});
        gl.uniform1i(gl.uniform(textured, "u_tex"), 0);
        begin(0, 0, 0);
        gl.op(GlOp::viewport, {0, 0, 8, 8});
        const std::vector<float> xy = {-1, -1, 1, -1, -1, 1, 1, -1, 1, 1, -1, 1}, uv = {0, 0, 1, 0, 0, 1, 1, 0, 1, 1, 0, 1};
        gl.draw(4, {{gl.attrib(textured, "a_xy"), {2, xy}}, {gl.attrib(textured, "a_uv"), {2, uv}}}, 6);
        gl.op(GlOp::viewport, {0, 0, W, H});
        check(frame() && near(at(img, 4, 60), 90, 0, 165), "minified texture samples mip level 3: got " + show(at(img, 4, 60)));
    }

    // the synthetic scene/blend programs exercise the same constructs as real content: they must build and render
    {
        const std::string dir = ATLAN_TEST_GLSL_DIR;
        const auto scene = gl.program(slurp(dir + "/scene.vert"), slurp(dir + "/scene.frag"));
        check(gl.linked(scene), "scene program links");
        gl.op(GlOp::use_program, {scene});
        gl.matrix4(gl.uniform(scene, "u_mvp"), identity());
        gl.matrix4(gl.uniform(scene, "u_view"), identity());
        gl.matrix4(gl.uniform(scene, "u_normal"), identity());
        gl.uniform1i(gl.uniform(scene, "u_lightCount"), 1);
        gl.uniform4(gl.uniform(scene, "u_lightPos[0]"), 0, 0, 1, 0);
        gl.uniform4(gl.uniform(scene, "u_diffuse[0]"), 0.5F, 0.5F, 0.5F, 1);
        gl.uniform4(gl.uniform(scene, "u_ambient"), 0.1F, 0.1F, 0.1F, 0);
        { GlCall one; one.op = GlOp::uniform_2fv; one.a = {gl.uniform(scene, "u_scale"), 1}; one.data = bytes_of<float>({1, 1}); backend.execute(one); }
        const auto white = gl.gen(GlOp::gen_texture);
        gl.op(GlOp::bind_texture, {0x0DE1, white});
        GlCall px; px.op = GlOp::tex_image_2d; px.a = {0x0DE1, 0, 0x1908, 1, 1, 0, 0x1908, 0x1401};
        for (int i = 0; i < 4; ++i) px.data.push_back(std::byte(255));
        backend.execute(px);
        gl.uniform1i(gl.uniform(scene, "u_tex"), 0);
        begin(0, 0, 0);
        // a_extra defaults to (0,0,0,1): vertex color rgb is 1, so the result is the lit white texel: (0.5 + 0.1) * 255
        gl.draw(4, {{gl.attrib(scene, "a_pos"), {3, cover_ccw}}, {gl.attrib(scene, "a_normal"), {3, {0, 0, 1, 0, 0, 1, 0, 0, 1}}}}, 3);
        check(frame() && near(at(img, 32, 32), 153, 153, 153, 3), "scene program: texture * (diffuse N.L + ambient): got " + show(at(img, 32, 32)));

        const auto mixer = gl.program(slurp(dir + "/blend.vert"), slurp(dir + "/blend.frag"));
        check(gl.linked(mixer), "blend program links");
        gl.op(GlOp::use_program, {mixer});
        const auto red = gl.gen(GlOp::gen_texture), green = gl.gen(GlOp::gen_texture);
        const auto upload = [&](std::uint32_t unit, std::uint32_t tex, unsigned char r, unsigned char g, unsigned char b) {
            gl.op(GlOp::active_texture, {0x84C0 + unit});
            gl.op(GlOp::bind_texture, {0x0DE1, tex});
            GlCall c; c.op = GlOp::tex_image_2d; c.a = {0x0DE1, 0, 0x1908, 1, 1, 0, 0x1908, 0x1401};
            for (unsigned char v : {r, g, b, (unsigned char)255}) c.data.push_back(std::byte(v));
            backend.execute(c);
            GlCall f; f.op = GlOp::tex_parameter_i; f.a = {0x0DE1, 0x2801, 0x2600}; backend.execute(f);
        };
        upload(0, red, 255, 0, 0);
        upload(1, green, 0, 255, 0);
        gl.uniform1i(gl.uniform(mixer, "u_a"), 0);
        gl.uniform1i(gl.uniform(mixer, "u_b"), 1);
        gl.matrix4(gl.uniform(mixer, "u_mvp"), identity());
        { GlCall f; f.op = GlOp::uniform_1f; f.a[0] = gl.uniform(mixer, "u_mixAmount"); f.f[0] = 0.25F; backend.execute(f); }
        gl.uniform1i(gl.uniform(mixer, "u_mode"), 0);
        const std::vector<float> uvs = {0, 0, 1, 0, 0, 1};
        const auto mix_draw = [&]() {
            begin(0, 0, 0);
            gl.draw(4, {{gl.attrib(mixer, "a_pos"), {3, cover_ccw}}, {gl.attrib(mixer, "a_uv0"), {2, uvs}}, {gl.attrib(mixer, "a_uv1"), {2, uvs}}}, 3);
            return frame();
        };
        check(mix_draw() && near(at(img, 8, 56), 191, 64, 0, 2), "blend program: mix(red, green, 0.25), two samplers: got " + show(at(img, 8, 56)));
        gl.uniform1i(gl.uniform(mixer, "u_swap"), 1);
        check(mix_draw() && near(at(img, 8, 56), 0, 64, 191, 2), "blend program: gl_FragColor read back and swapped: got " + show(at(img, 8, 56)));
        check(backend.stats().library_failures == 0 && backend.stats().pipeline_failures == 0, "no Metal library or pipeline failures so far");
    }

    // MarginGlBackend: a 64-pixel guest surface in an 80-pixel target with a 16-pixel margin on the left
    {
        atlan::MetalGlBackend wide(80, 32);
        atlan::MarginGlBackend margin(wide);
        margin.set_layout(64, 16, 0);
        margin.set_scene_names("a_pos", "u_m");
        Driver mg{margin};
        const auto p = mg.program("attribute vec3 a_pos; uniform mat4 u_m; void main(){ gl_Position = u_m * vec4(a_pos, 1.0); }", flat_fs);
        check(mg.linked(p), "rewritten program links");
        mg.op(GlOp::use_program, {p});
        mg.matrix4(mg.uniform(p, "u_m"), gltest::identity());
        const auto c = mg.uniform(p, "u_color"), a = mg.attrib(p, "a_pos");
        std::vector<std::uint8_t> px;
        std::uint32_t pw = 0, ph = 0;
        const auto pixel = [&](std::uint32_t x, std::uint32_t y) { const auto* q = &px[(std::size_t{y} * pw + x) * 4]; return Pixel{q[2], q[1], q[0], q[3]}; };
        mg.op(GlOp::viewport, {0, 0, 64, 32});
        mg.clear_color(0, 0, 1, 1);
        mg.op(GlOp::clear, {0x4100});
        mg.uniform4(c, 1, 0, 0, 1);
        mg.draw(5, {{a, {3, {-1.5F, -1, 0, 0, -1, 0, -1.5F, 1, 0, 0, 1, 0}}}}, 4);
        mg.op(GlOp::swap_buffers);
        check(wide.read_back(px, pw, ph) && pw == 80 && ph == 32, "margin frame read back");
        if (pw == 80) {
            check(near(pixel(2, 16), 255, 0, 0), "past the guest's left edge shows in the margin: " + show(pixel(2, 16)));
            check(near(pixel(46, 16), 255, 0, 0) && near(pixel(50, 16), 0, 0, 255), "guest pixels keep their place: " + show(pixel(46, 16)) + show(pixel(50, 16)));
        }
        mg.op(GlOp::clear, {0x4100});
        mg.uniform4(c, 0, 1, 0, 1);
        mg.draw(5, {{a, {3, {-1, -1, 0, 1, -1, 0, -1, 1, 0, 1, 1, 0}}}}, 4);
        mg.op(GlOp::swap_buffers);
        check(wide.read_back(px, pw, ph) && pw == 80 && near(pixel(1, 16), 0, 255, 0) && near(pixel(78, 16), 0, 255, 0),
              "a full-width backdrop continues over the margin");
        mg.op(GlOp::clear, {0x4100});
        mg.uniform4(c, 1, 0, 0, 1);
        mg.draw(5, {{a, {3, {-1, -1, 0, -0.5F, -1, 0, -1, 1, 0, -0.5F, 1, 0}}}}, 4);
        mg.op(GlOp::swap_buffers);
        check(wide.read_back(px, pw, ph) && pw == 80 && near(pixel(8, 16), 0, 0, 255) && near(pixel(18, 16), 255, 0, 0),
              "a narrow panel on the left edge stays out of the margin");
    }

    // every program in a shader container builds a Metal pipeline
    if (const char* dat = std::getenv("ATLAN_SHADER_DAT")) {
        const auto blob = slurp(dat);
        check(blob.size() > 4, "shader container is readable");
        std::uint32_t count = 0;
        if (blob.size() > 4) std::memcpy(&count, blob.data(), 4);
        for (std::uint32_t i = 0; i < count; ++i) {
            std::uint32_t entry[4];
            if (4 + 8 * (2 * std::size_t{i} + 2) > blob.size()) { check(false, "container entry table is truncated"); break; }
            std::memcpy(entry, blob.data() + 4 + 16 * std::size_t{i}, 16);
            if (std::size_t{entry[1]} + entry[0] > blob.size() || std::size_t{entry[3]} + entry[2] > blob.size()) { check(false, "container entry is out of range"); break; }
            const std::string vs = blob.substr(entry[1], entry[0]), fs = blob.substr(entry[3], entry[2]);
            const auto p = gl.program(vs, fs);
            check(gl.linked(p), "container program " + std::to_string(i) + " links");
            gl.op(GlOp::use_program, {p});
            std::vector<std::pair<std::uint32_t, std::pair<std::uint32_t, std::vector<float>>>> attribs;
            const auto active = gl.op(GlOp::get_program_iv, {p, 0x8B89}).value;
            for (std::uint32_t k = 0; k < active; ++k) attribs.push_back({k, {4, {0, 0, 0, 1, 0, 0, 0, 1, 0, 0, 0, 1}}});
            gl.draw(4, attribs, 3);
        }
        gl.op(GlOp::swap_buffers);
        const auto st = backend.stats();
        check(st.library_failures == 0 && st.pipeline_failures == 0, "all container programs compile and build a pipeline");
        for (const auto& l : log) std::cerr << "log: " << l << '\n';
    }
    for (const auto& l : log) std::cerr << "log: " << l << '\n';
    if (failed) return 1;
    std::cout << "metal render tests passed: " << checks << " checks\n";
}
