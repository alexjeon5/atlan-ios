// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include "atlan/imports.hpp"
#include <array>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace atlan {
// GLES2 / EGL calls as the guest makes them, decoded into host values. The service resolves guest pointers
// (client-side vertex arrays and indices, texture pixels, shader sources, uniform arrays) into `data`/`text`
// so a backend (Metal on Apple hosts, a recorder in tests) never touches guest memory.
enum class GlOp : std::uint8_t {
    active_texture, attach_shader, bind_buffer, bind_texture, blend_func, buffer_data, clear, clear_color,
    clear_depthf, compile_shader, compressed_tex_image_2d, create_program, create_shader, cull_face,
    delete_buffer, delete_program, delete_shader, delete_texture, depth_func, depth_mask, disable,
    disable_vertex_attrib_array, draw_arrays, draw_elements, enable, enable_vertex_attrib_array, finish,
    front_face, gen_buffer, gen_texture, get_attrib_location, get_error, get_program_iv, get_shader_info_log,
    get_shader_iv, get_string, get_uniform_location, is_texture, line_width, link_program, pixel_store_i,
    read_pixels, shader_source, tex_image_2d, tex_parameter_f, tex_parameter_i, uniform_1f, uniform_1i,
    uniform_2fv, uniform_4fv, uniform_matrix_4fv, use_program, vertex_attrib_pointer, viewport, swap_buffers
};

struct GlAttribData {  // one enabled client-side vertex attribute gathered for a draw call
    std::uint32_t index{}, size{}, type{}, stride{};
    bool normalized{};
    std::vector<std::byte> bytes;  // vertices [0, vertex_count) starting at the attribute's pointer
};

struct GlCall {
    GlOp op{};
    std::array<std::uint32_t, 8> a{};  // integer/enum/id arguments in guest order (floats in `f`)
    std::array<float, 4> f{};
    std::vector<std::byte> data;       // buffer/texture/uniform/index payload
    std::string text;                  // shader source (concatenated) or name
    std::vector<GlAttribData> attribs; // client-side attributes for draw calls
    std::uint32_t vertex_count{};      // draw calls: number of vertices the attributes cover
    std::uint32_t id{};                // object id the service allocated for create/gen ops
};

struct GlResult {
    std::uint32_t value{};
    std::vector<std::byte> bytes;  // read_pixels data
    std::string text;              // info log / string
};

class GlBackend {
public:
    virtual ~GlBackend() = default;
    virtual GlResult execute(const GlCall& call) = 0;
};

// Accepts every call; reports success for compile/link queries and no errors. Used until a device backend exists.
class NullGlBackend final : public GlBackend {
public:
    GlResult execute(const GlCall& call) override {
        GlResult r;
        if (call.op == GlOp::get_program_iv || call.op == GlOp::get_shader_iv) r.value = 1;  // status = true
        if (call.op == GlOp::get_attrib_location || call.op == GlOp::get_uniform_location) r.value = 0;
        if (call.op == GlOp::get_string) r.text = "atlan null renderer";
        ++calls;
        return r;
    }
    std::uint64_t calls{};
};

class Gles2 {
public:
    struct Config {
        std::uint32_t string_area{};  // mapped guest scratch (>= 512 bytes) for glGetString results
    };
    Gles2(GlBackend& backend, Config config) : backend_(backend), config_(config) {}
    void install(ImportDispatcher& dispatcher);
private:
    struct Attrib {
        bool enabled{};
        std::uint32_t size{}, type{}, stride{}, pointer{}, buffer{};
        bool normalized{};
    };
    [[nodiscard]] std::optional<std::string> gather(ImportCall& call, GlCall& out, std::uint32_t vertex_count);
    GlBackend& backend_;
    Config config_;
    std::uint32_t next_id_{1};
    std::uint32_t array_buffer_{}, element_buffer_{};
    std::array<Attrib, 16> attribs_{};
    friend struct GlesBinder;
};
}
