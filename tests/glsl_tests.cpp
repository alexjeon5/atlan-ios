// SPDX-License-Identifier: GPL-2.0-or-later
#include "atlan/glsl.hpp"
#include <cstdlib>
#include <fstream>
#include <iostream>
#include <iterator>
#include <string>

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
using namespace atlan::glsl;
bool has(const std::string& text, const std::string& part) { return text.find(part) != std::string::npos; }
ShaderUnit vs(const std::string& source) { return compile(Stage::vertex, source); }
ShaderUnit fs(const std::string& source) { return compile(Stage::fragment, source); }
}

int main(int argc, char** argv) {
    const std::string dir = argc > 1 ? argv[1] : "tests/glsl";
    {
        const auto v = vs(slurp(dir + "/scene.vert")), f = fs(slurp(dir + "/scene.frag"));
        check(v.ok && f.ok, "synthetic scene shaders compile");
        check(v.attributes.size() == 4 && v.attributes[3].name == "a_extra" && v.attributes[3].type.rows == 4, "attributes reflected in order");
        check(f.samplers.size() == 1 && f.samplers[0].name == "u_tex", "sampler reflected");
        const auto p = link(v, f);
        check(p.ok, "scene program links");
        check(p.attribute_location("a_pos") == 0 && p.attribute_location("a_uv") == 2 && p.attribute_location("nope") == ~0u, "attribute locations");
        check(p.uniform_location("u_mvp") == 0 && p.uniform_location("u_view") == 1, "uniform locations follow declaration order");
        const auto light = p.uniform_location("u_lightPos");
        check(light != ~0u && p.uniform_location("u_lightPos[0]") == light && p.uniform_location("u_lightPos[1]") == light + 1, "array element locations");
        check(p.uniform_location("u_lightPos[2]") == ~0u && p.uniform_location("u_lightPos[x]") == ~0u, "array bounds enforced");
        check(p.uniform_location("u_tex") == p.sampler_location_base, "sampler location follows the uniforms");
        check(p.uniform_location("missing") == ~0u, "unknown uniform");
        std::uint32_t last_end = 0;
        for (const auto& u : p.uniforms) {
            check(u.offset % 16 == 0 && u.offset >= last_end, "uniform offsets are 16-byte aligned and ordered");
            last_end = u.offset + u.stride * u.count();
        }
        check(p.uniform_bytes >= last_end && p.uniform_bytes % 16 == 0, "block size covers every uniform");
        check(has(p.msl, "float4(float3(1.0f, 1.0f, 1.0f), ((a.a_extra / 100.0f)).x)"), "extra constructor arguments are dropped");
        check(has(p.msl, "_glsl_meq(u.u_view, _glsl_mat4(1.0f))"), "matrix equality uses the helper");
        check(has(p.msl, "a.a_uv.xy"), "stpq swizzle maps to xyzw");
        check(has(p.msl, "t_u_tex.sample(m_u_tex,"), "texture2D samples through the bound pair");
        check(has(p.msl, "float fs_fogFactor(constant Uniforms& u, thread VOut& v, thread FOut& f, texture2d<float> t_u_tex, sampler m_u_tex)"),
              "helpers receive the stage environment");
        check(has(p.msl, "discard_fragment()"), "discard maps to discard_fragment");
        check(has(p.msl, "o.gl_Position.z = (o.gl_Position.z + o.gl_Position.w) * 0.5f"), "clip-space depth remapped to Metal's range");
    }
    {
        const auto v = vs("attribute vec2 a_xy;\nvoid main(){ gl_Position = vec4(a_xy, 0.0, 1.0); }\n");
        const auto f = fs("precision mediump float;\nvarying vec2 v_missing;\nvoid main(){ gl_FragColor = vec4(v_missing, 0.0, 1.0); }\n");
        const auto p = link(v, f);
        check(p.ok && has(p.log, "v_missing") && has(p.msl, "float2 v_missing;"), "fragment-only varying links as a zero-filled output");
    }
    {
        const auto v = vs("uniform vec4 c;\nattribute vec4 p;\nvoid main(){ gl_Position = p * c; }\n");
        const auto f = fs("uniform vec3 c;\nvoid main(){ gl_FragColor = vec4(c, 1.0); }\n");
        const auto p = link(v, f);
        check(!p.ok && has(p.log, "different types"), "uniform type mismatch fails the link");
    }
    // compile errors carry a line number and a reason
    {
        auto u = fs("void main(){\n  gl_FragColor = vec4(foo);\n}\n");
        check(!u.ok && has(u.log, "0:2") && has(u.log, "undeclared identifier 'foo'"), "undeclared identifier");
        u = fs("void helper(){}\n");
        check(!u.ok && has(u.log, "missing main"), "missing main");
        u = fs("struct S { float a; };\nvoid main(){}\n");
        check(!u.ok && has(u.log, "struct"), "struct is rejected");
        u = fs("void main(){ vec3 a = vec3(1.0); a = vec4(1.0); }\n");
        check(!u.ok && has(u.log, "cannot assign"), "size mismatch on assignment");
        u = fs("void main(){ if (1.0) {} }\n");
        check(!u.ok && has(u.log, "condition must be a bool"), "non-bool condition");
        u = fs("void main(){ gl_FragColor = vec4(1.0).xyzzy; }\n");
        check(!u.ok && has(u.log, "swizzle"), "swizzle longer than four");
        u = fs("void main(){ gl_FragColor = vec4(1.0).rgz; }\n");
        check(!u.ok && has(u.log, "swizzle"), "mixed swizzle sets");
        u = vs("void main(){ gl_Position = vec4(1.0) \n}");
        check(!u.ok && has(u.log, "expected ';'"), "missing semicolon");
        u = fs("void main(){ gl_FragColor = vec4(unknownFn(1.0)); }\n");
        check(!u.ok && has(u.log, "unknown function 'unknownFn'"), "unknown function");
        u = fs("void main(){ gl_FragColor = vec4(1.0); }\n#warning x\n");
        check(!u.ok && has(u.log, "unsupported preprocessor"), "unsupported directive");
        u = vs("void main(){ gl_FragColor = vec4(1.0); }\n");
        check(!u.ok, "gl_FragColor is not visible in a vertex shader");
        u = fs("attribute vec4 a;\nvoid main(){}\n");
        check(!u.ok, "attributes are vertex-only");
    }
    // accepted constructs
    {
        auto u = fs("#version 100\n#ifdef GL_ES\nprecision highp float;\n#endif\n#define K 0.25\n"
                    "uniform float f;\nvoid main(){ float x = K + f; int n = 2; x += float(n); gl_FragColor = vec4(x); }\n");
        check(u.ok, "version, ifdef GL_ES and object-like macros");
        u = fs("float twice(float x){ return x * 2.0; }\nvoid swap(inout float a, out float b){ b = a; a = 0.0; }\n"
               "void main(){ float a = 1.0, b; swap(a, b); gl_FragColor = vec4(twice(a + b)); }\n");
        check(u.ok && has(u.functions, "thread float& a"), "user functions with inout/out parameters");
        u = fs("void main(){ int i = 0; while (i < 3) { i++; } do { i--; } while (i > 0); gl_FragColor = vec4(float(i)); }\n");
        check(u.ok, "while and do-while");
        u = fs("void main(){ vec2 p = vec2(1.0, 2.0); gl_FragColor = vec4(p, p.yx); }\n");
        check(u.ok, "vector constructors from vectors");
        u = fs("void main(){ mat2 m = mat2(1.0, 0.0, 0.0, 1.0); vec2 r = m * vec2(1.0); gl_FragColor = vec4(r, m[1]); }\n");
        check(u.ok, "matrix constructors and products");
        u = fs("void main(){ gl_FragColor = vec4(0.0); if (gl_FragColor.x < 1.0 || !(false)) { gl_FragColor.y = 1.0; } }\n");
        check(u.ok, "logic operators and component assignment");
        u = fs("uniform sampler2D t;\nvarying vec3 q;\nvoid main(){ gl_FragColor = texture2DProj(t, q) + texture2D(t, q.xy); }\n");
        check(u.ok && has(u.functions, "/ v.q.z"), "texture2DProj divides by the third component");
    }
    std::cout << "glsl tests passed: " << checks << " checks\n";
}
