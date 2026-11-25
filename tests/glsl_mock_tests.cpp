// SPDX-License-Identifier: GPL-2.0-or-later
// Compiles the MSL that atlan::glsl produces for the synthetic test shaders as C++ against tests/mock_metal and runs
// the vertex and fragment entry points. Clang only (the mock leans on ext_vector_type and elementwise builtins).
#include "atlan/glsl.hpp"
#include <cmath>
#include <cstddef>
#include <cstdio>
#include <cstdlib>
#include <iostream>

// Before the mock: it defines `vertex`/`fragment` as empty macros.
static atlan::glsl::Linked link_sources(const char* vertex_source, const char* fragment_source) {
    return atlan::glsl::link(atlan::glsl::compile(atlan::glsl::Stage::vertex, vertex_source),
                             atlan::glsl::compile(atlan::glsl::Stage::fragment, fragment_source));
}
#include <metal_stdlib>

namespace scene {
#include "scene.metal"
}
namespace flat {
#include "flat.metal"
}

namespace {
int checks = 0;
void check(bool ok, const char* what) {
    ++checks;
    if (!ok) { std::cerr << "FAILED: " << what << '\n'; std::exit(1); }
}
bool near(float a, float b) { return std::fabs(a - b) < 1e-5f; }
bool near4(metal::float4 v, float x, float y, float z, float w) { return near(v.x, x) && near(v.y, y) && near(v.z, z) && near(v.w, w); }
scene::float4x4 identity() {
    return scene::float4x4(metal::float4{1, 0, 0, 0}, metal::float4{0, 1, 0, 0}, metal::float4{0, 0, 1, 0}, metal::float4{0, 0, 0, 1});
}
}

int main() {
    using namespace metal;
    // ---- scene program: vertex stage
    scene::Uniforms u{};
    u.u_mvp = identity();
    u.u_view = identity();
    u.u_normal = identity();
    u.u_tint = float4{1, 1, 1, 1};
    scene::VIn in{};
    in.a_pos = float4{0.5f, -0.25f, 0.5f, 1};
    in.a_normal = float3{0, 3, 4};
    in.a_uv = float2{0.25f, 0.75f};
    in.a_extra = float4{50, 0, 0, 0};
    auto out = scene::vs_main(in, u);
    check(near4(out.gl_Position, 0.5f, -0.25f, 0.75f, 1), "position passes through and z maps to [0,1]");
    check(near(out.v_pos.x, 0.5f) && near(out.v_pos.z, 0.5f), "vec3(mat4 * vec4) truncates");
    check(near4(out.v_color, 1, 1, 1, 0.5f), "vec4(vec3, vec4) takes one component from the extra vector");
    check(near(out.v_normal.y, 0.6f) && near(out.v_normal.z, 0.8f), "normal is transformed and normalized");
    check(near(out.v_flat, 1.0f) && near(out.v_uv.x, 0.25f) && near(out.v_uv.y, 0.75f), "identity detection and st swizzle");
    u.u_view[0][0] = 2.0f;
    u.u_eye = float4{0, 0, 1, 0};
    out = scene::vs_main(in, u);
    check(near(out.v_flat, 0.0f) && near4(out.v_eye, 1.0f, -0.25f, -0.5f, 1.0f), "non-identity view matrix branch");

    // ---- scene program: fragment stage
    scene::Uniforms fu{};
    scene::VOut v{};
    v.v_pos = float3{0, 0, 0};
    v.v_color = float4{1, 1, 1, 0.5f};
    v.v_normal = float3{0, 0, 1};
    v.v_uv = float2{0.5f, 0.25f};
    v.v_flat = 1.0f;
    fu.u_scale = float2{2, 4};
    fu.u_ambient = float4{0.1f, 0.1f, 0.1f, 0};
    fu.u_lightPos[0] = float4{0, 0, 1, 0};
    fu.u_lightPos[1] = float4{0, 0, 3, 1};
    fu.u_diffuse[0] = float4{0.5f, 0.5f, 0.5f, 1};
    fu.u_diffuse[1] = float4{0.25f, 0.25f, 0.25f, 1};
    fu.u_fogNear = 0;
    fu.u_fogFar = 10;
    fu.u_fogColor = float4{1, 0, 0, 1};
    float2 sampled_uv{};
    texture2d<float> tex;
    tex.fetch = [&](float2 uv) { sampled_uv = uv; return float4{0.5f, 0.25f, 1, 1}; };
    sampler smp;
    g_discarded = false;
    auto frag = scene::fs_main(v, fu, tex, smp);
    check(near(sampled_uv.x, 1.0f) && near(sampled_uv.y, 1.0f), "texture coordinate is scaled before sampling");
    check(near4(frag.gl_FragColor, 0.5f, 0.25f, 1.0f, 0.5f), "unlit color is texel * vertex color");
    fu.u_lightCount = 1;
    frag = scene::fs_main(v, fu, tex, smp);
    check(near4(frag.gl_FragColor, 0.5f * 0.6f, 0.25f * 0.6f, 0.6f, 0.5f), "directional light plus ambient, alpha preserved");
    fu.u_lightCount = 2;
    frag = scene::fs_main(v, fu, tex, smp);
    check(near4(frag.gl_FragColor, 0.5f * 0.85f, 0.25f * 0.85f, 0.85f, 0.5f), "second (point) light accumulates in the loop");
    fu.u_lightCount = 0;
    fu.u_fog = 1;
    v.v_flat = 0.0f;
    v.v_eye = float4{0, 0, 5, 0};
    frag = scene::fs_main(v, fu, tex, smp);
    check(near4(frag.gl_FragColor, 0.75f, 0.125f, 0.5f, 0.5f), "linear fog mixes toward the fog color");
    fu.u_fog = 0;
    fu.u_clip = 1;
    v.v_pos = float3{2, 0, 0};
    frag = scene::fs_main(v, fu, tex, smp);
    check(g_discarded, "discard is raised for clipped fragments");

    // ---- flat program
    flat::Uniforms flu{};
    flu.u_color = float4{1, 1, 1, 1};
    flu.u_enabled = 1;
    flat::VOut fv{};
    auto fframe = flat::fs_main(fv, flu);
    check(near4(fframe.gl_FragColor, 0.5f, 0.5f, 0.5f, 0.5f), "bool uniform, const and vector * scalar");
    flu.u_enabled = 0;
    fframe = flat::fs_main(fv, flu);
    check(near4(fframe.gl_FragColor, 3, 0, 0, 1), "mod() helper and ternary");
    flat::VIn fin{};
    fin.a_xy = float2{-1, 1};
    check(near4(flat::vs_main(fin, flu).gl_Position, -1, 1, 0.5f, 1), "2-component attribute widened to vec4 position");

    // ---- layout of the compiled struct matches the reflection the GL layer uses
    const auto program = link_sources("uniform mat4 u_mvp;uniform mat4 u_view;uniform mat4 u_normal;uniform vec4 u_tint;uniform vec4 u_eye;attribute vec4 a_pos;attribute vec3 a_normal;attribute vec2 a_uv;attribute vec4 a_extra;void main(){gl_Position=u_mvp*a_pos+u_view*a_pos+u_normal*a_pos+u_tint+u_eye+vec4(a_normal,0.0)+vec4(a_uv,0.0,0.0)+a_extra;}",
                                 "uniform vec4 u_lightPos[2];uniform vec4 u_diffuse[2];uniform vec4 u_ambient;uniform int u_lightCount;uniform float u_fogNear;uniform float u_fogFar;uniform vec4 u_fogColor;uniform int u_fog;uniform int u_clip;uniform vec2 u_scale;void main(){gl_FragColor=u_lightPos[1]+u_diffuse[1]+u_ambient+u_fogColor+vec4(float(u_lightCount+u_fog+u_clip)+u_fogNear+u_fogFar+u_scale.x);}");
    check(program.ok, "reflection probe links");
    const auto offset_of = [&](const char* name) { for (const auto& s : program.uniforms) if (s.name == name) return s.offset; return ~0u; };
    check(offset_of("u_mvp") == offsetof(scene::Uniforms, u_mvp) && offset_of("u_eye") == offsetof(scene::Uniforms, u_eye), "matrix and vec4 offsets");
    check(offset_of("u_lightPos") == offsetof(scene::Uniforms, u_lightPos) && offset_of("u_lightCount") == offsetof(scene::Uniforms, u_lightCount), "array and int offsets");
    check(offset_of("u_scale") == offsetof(scene::Uniforms, u_scale) && program.uniform_bytes == sizeof(scene::Uniforms), "block size");
    std::cout << "glsl mock-metal tests passed: " << checks << " checks\n";
}
