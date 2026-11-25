// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include <cstdint>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

// GLSL ES 1.00 -> Metal Shading Language source translator.
//
// A shader is compiled on its own (compile()) into a ShaderUnit: the declarations it makes plus its function bodies
// already rewritten as MSL text. link() merges a vertex and a fragment unit into one MSL library source and a
// reflection record (uniform block layout, attribute locations, sampler slots) that the GL layer needs to answer
// glGetUniformLocation / glGetAttribLocation and to marshal uniform values.
//
// Supported: attribute/varying/uniform/const declarations, float/int/bool scalars, vec/ivec/bvec/mat2-4, arrays,
// user functions (in/out/inout), if/for/while/do, discard, constructors with GLSL flattening rules, swizzles
// (xyzw/rgba/stpq), sampler2D with texture2D/texture2DProj/texture2DLod, and the usual math built-ins.
// Not supported (reported as compile errors): structs, global non-const variables, samplerCube, derivatives,
// gl_FragCoord/gl_PointCoord, vertex texture fetch.
namespace atlan::glsl {
enum class Stage : std::uint8_t { vertex, fragment };
enum class Base : std::uint8_t { void_, bool_, int_, float_, sampler2d };

struct Type {
    Base base{Base::void_};
    std::uint8_t rows{1};      // vector length or matrix row count
    std::uint8_t cols{1};      // matrix column count (1 unless matrix)
    std::uint16_t array{0};    // element count, 0 = not an array
    bool operator==(const Type&) const = default;
    [[nodiscard]] bool is_scalar() const noexcept { return array == 0 && rows == 1 && cols == 1 && base != Base::void_ && base != Base::sampler2d; }
    [[nodiscard]] bool is_vector() const noexcept { return array == 0 && rows > 1 && cols == 1; }
    [[nodiscard]] bool is_matrix() const noexcept { return array == 0 && cols > 1; }
    [[nodiscard]] std::uint32_t components() const noexcept { return std::uint32_t{rows} * cols; }
    [[nodiscard]] Type element() const noexcept { Type t = *this; t.array = 0; return t; }
    [[nodiscard]] std::string msl() const;   // element type name in MSL
    [[nodiscard]] std::string glsl() const;  // element type name in GLSL
};

struct Declaration {
    std::string name;   // GLSL name
    Type type;
};

struct ShaderUnit {
    Stage stage{Stage::vertex};
    bool ok{};
    std::string log;
    std::vector<Declaration> uniforms, attributes, varyings, samplers;
    std::string functions;  // helper functions and the renamed main(), as MSL text
};

[[nodiscard]] ShaderUnit compile(Stage stage, std::string_view source);

struct UniformSlot {
    std::string name;
    Type type;                   // element type with `array` = element count (0 for a single value)
    std::uint32_t offset{};      // byte offset in the uniform block
    std::uint32_t stride{};      // bytes per element (MSL sizeof of the element type)
    std::uint32_t location{};    // first location; array element i is location + i
    [[nodiscard]] std::uint32_t count() const noexcept { return type.array ? type.array : 1; }
};

struct Linked {
    bool ok{};
    std::string log;
    std::string msl;                     // complete library: entry points are `vs_main` and `fs_main`
    std::vector<UniformSlot> uniforms;   // non-sampler uniforms
    std::uint32_t uniform_bytes{};       // size of the uniform block (also the size of the `Uniforms` MSL struct)
    std::vector<Declaration> attributes; // attribute i has location i and is MSL [[attribute(i)]]
    std::vector<Declaration> samplers;   // sampler i uses [[texture(i)]] / [[sampler(i)]]; its location follows the uniforms
    std::uint32_t sampler_location_base{};
    // Location of "name", "name[0]" or "name[i]"; ~0u when no such active uniform exists.
    [[nodiscard]] std::uint32_t uniform_location(std::string_view name) const;
    [[nodiscard]] std::uint32_t attribute_location(std::string_view name) const;  // ~0u when absent
};

[[nodiscard]] Linked link(const ShaderUnit& vertex, const ShaderUnit& fragment);
}
