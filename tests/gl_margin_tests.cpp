// SPDX-License-Identifier: GPL-2.0-or-later
// MarginGlBackend: the vertex shader rewrite, the viewport and uniform it sends, and that the state machine (the
// Metal path's translator) accepts the rewritten shaders. Pixels are checked in gles-backend and metal-render.
#include "atlan/gl_context.hpp"
#include "atlan/gl_margin.hpp"
#include "gl_driver.hpp"
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <iostream>
#include <iterator>

using namespace gltest;
namespace {
int checks = 0;
void check(bool ok, const std::string& what) {
    ++checks;
    if (!ok) { std::cerr << "FAILED: " << what << '\n'; std::exit(1); }
}
std::string slurp(const std::string& path) {
    std::ifstream in(path, std::ios::binary);
    check(bool(in), "cannot open " + path);
    return {std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>()};
}
// Records what reaches the wrapped backend; the margin uniform is at location 7.
struct Recorder final : GlBackend {
    std::vector<GlCall> calls;
    GlResult execute(const GlCall& c) override {
        calls.push_back(c);
        GlResult r;
        if (c.op == GlOp::get_uniform_location && c.text == MarginGlBackend::uniform_name) r.value = 7;
        return r;
    }
    std::vector<GlCall> of(GlOp op) const {
        std::vector<GlCall> out;
        for (const auto& c : calls) if (c.op == op) out.push_back(c);
        return out;
    }
};
std::array<float, 4> floats(const GlCall& c) {
    std::array<float, 4> v{};
    std::memcpy(v.data(), c.data.data(), std::min(c.data.size(), sizeof v));
    return v;
}
bool close(float a, float b) { const float d = a - b, tol = 1e-5F * (1 + (a < 0 ? -a : a)); return d < tol && -d < tol; }
}

int main(int argc, char** argv) {
    const std::string dir = argc > 1 ? argv[1] : "tests/glsl";
    {   // the rewrite
        const std::string plain = "attribute vec4 p;\nvoid main()\n{\n\tgl_Position = p;\n}\n";
        const auto patched = MarginGlBackend::patch_vertex_shader(plain);
        check(patched.find("void atlan_guest_main()") != std::string::npos, "main is renamed");
        check(patched.find("uniform highp vec4 atlan_margin_x;") != std::string::npos, "the host uniform is declared");
        check(patched.rfind("void main()") > patched.find("atlan_guest_main()"), "a new main follows");
        const auto commented = MarginGlBackend::patch_vertex_shader("// void main() here\n/* void main( */ void  main ( void ) { gl_Position = vec4(0.0); }");
        check(commented.find("// void main() here") == 0 && commented.find("void  atlan_guest_main ( void )") != std::string::npos,
              "comments are skipped: " + commented);
        check(MarginGlBackend::patch_vertex_shader("void mainly() {}") == "void mainly() {}", "no main, no change");
    }
    {   // viewport and uniform
        Recorder inner;
        MarginGlBackend margin(inner);
        Driver gl{margin};
        margin.set_layout(800, 60, 0);
        const auto prog = gl.program("attribute vec4 p; void main() { gl_Position = p; }", "void main() { gl_FragColor = vec4(1.0); }");
        gl.op(GlOp::use_program, {prog});
        gl.op(GlOp::viewport, {0, 0, 800, 480});
        auto viewports = inner.of(GlOp::viewport);
        check(viewports.size() == 1 && viewports[0].a[0] == 0 && viewports[0].a[2] == 860 && viewports[0].a[3] == 480,
              "a full-surface viewport covers the margin");
        gl.op(GlOp::draw_arrays, {4, 0, 3});
        auto uniforms = inner.of(GlOp::uniform_4fv);
        check(uniforms.size() == 1 && uniforms[0].a[0] == 7, "the transform is set before the draw");
        auto t = floats(uniforms[0]);
        check(close(t[0], 800.0F / 860) && close(t[1], 60.0F / 860) && t[2] == 1 && t[3] == 0, "scale, offset and left snap");
        // guest NDC -1 lands on target pixel 60, guest NDC 1 on 860
        check(close((-t[0] + t[1] + 1) / 2 * 860, 60) && close((t[0] + t[1] + 1) / 2 * 860, 860), "guest pixels keep their place");
        gl.op(GlOp::draw_arrays, {4, 0, 3});
        check(inner.of(GlOp::uniform_4fv).size() == 1, "an unchanged transform is not sent again");
        gl.op(GlOp::viewport, {100, 50, 200, 100});
        viewports = inner.of(GlOp::viewport);
        check(viewports.back().a[0] == 160 && viewports.back().a[1] == 50 && viewports.back().a[2] == 200, "an inner viewport only moves");
        gl.op(GlOp::draw_arrays, {4, 0, 3});
        t = floats(inner.of(GlOp::uniform_4fv).back());
        check(t[0] == 1 && t[1] == 0 && t[2] == 0 && t[3] == 0, "and draws untransformed");
        const auto read = gl.op(GlOp::read_pixels, {0, 0, 800, 480, 0x1908, 0x1401});
        (void)read;
        check(inner.of(GlOp::read_pixels).back().a[0] == 60, "read-backs shift by the left margin");
        margin.set_layout(800, 0, 60);  // turned over: the margin is on the right
        gl.op(GlOp::draw_arrays, {4, 0, 3});
        viewports = inner.of(GlOp::viewport);
        check(viewports.back().a[0] == 100 && viewports.back().a[2] == 200, "a new layout re-applies the viewport");
        gl.op(GlOp::viewport, {0, 0, 800, 480});
        gl.op(GlOp::draw_arrays, {4, 0, 3});
        t = floats(inner.of(GlOp::uniform_4fv).back());
        check(inner.of(GlOp::viewport).back().a[2] == 860 && close(t[1], -60.0F / 860) && t[2] == 0 && t[3] == 1, "right margin");
        margin.set_layout(800, 0, 0);
        gl.op(GlOp::draw_arrays, {4, 0, 3});
        t = floats(inner.of(GlOp::uniform_4fv).back());
        check(inner.of(GlOp::viewport).back().a[2] == 800 && t == std::array<float, 4>{1, 0, 0, 0}, "no margins: the guest's own viewport");
        const auto relinked = inner.of(GlOp::uniform_4fv).size();
        gl.op(GlOp::link_program, {prog});
        gl.op(GlOp::draw_arrays, {4, 0, 3});
        check(inner.of(GlOp::uniform_4fv).size() == relinked + 1, "a relinked program gets the transform again");
    }
    {   // the state machine translates rewritten shaders and passes the uniform to the draw
        struct Sink final : GlSink {
            std::vector<GlDraw> draws;
            void clear(const GlClear&) override {}
            void draw(const GlDraw& d) override { draws.push_back(d); }
            void present() override {}
        } sink;
        GlContext context(sink);
        std::vector<std::string> log;
        context.set_log([&](const std::string& s) { log.push_back(s); });
        MarginGlBackend margin(context);
        margin.set_layout(48, 16, 0);
        Driver gl{margin};
        const auto prog = gl.program(slurp(dir + "/scene.vert"), slurp(dir + "/scene.frag"));
        for (const auto& l : log) std::cerr << "log: " << l << '\n';
        check(gl.linked(prog), "a rewritten guest-style shader links");
        gl.op(GlOp::use_program, {prog});
        gl.op(GlOp::viewport, {0, 0, 48, 64});
        gl.op(GlOp::draw_arrays, {4, 0, 3});
        check(sink.draws.size() == 1 && sink.draws[0].state.viewport[2] == 64, "the draw has the widened viewport");
        const auto location = gl.uniform(prog, MarginGlBackend::uniform_name);
        check(location != ~0u, "the margin uniform has a location");
    }
    std::cout << "gl margin tests passed: " << checks << " checks\n";
}
