// SPDX-License-Identifier: GPL-2.0-or-later
#include "atlan/gles.hpp"
#include "atlan/abi.hpp"
#include <algorithm>
#include <bit>
#include <cstring>

namespace atlan {
namespace {
constexpr std::uint32_t array_buffer = 0x8892, element_buffer = 0x8893;
constexpr std::uint32_t ubyte = 0x1401, ushort = 0x1403, uint_ = 0x1405;
constexpr std::size_t max_payload = 256U << 20;
float as_float(std::uint32_t bits) { return std::bit_cast<float>(bits); }
std::uint32_t type_size(std::uint32_t type) {
    switch (type) {
        case 0x1400: case ubyte: return 1;
        case 0x1402: case ushort: return 2;
        case 0x1404: case uint_: case 0x1406: case 0x140C: return 4;
    }
    return 0;
}
using Err = std::optional<std::string>;
// Word arguments including stack-passed ones (ImportCall::arg covers r0-r3 only).
bool load_args(ImportCall& c, unsigned count, std::array<std::uint32_t, 10>& out) {
    for (unsigned i = 0; i < count; ++i)
        if (!c.read_arg(i, out[i])) return false;
    return true;
}
bool read_bytes(ImportCall& c, std::uint32_t address, std::uint64_t size, std::vector<std::byte>& out) {
    if (size > max_payload) return false;
    out.resize(static_cast<std::size_t>(size));
    return size == 0 || c.runtime().memory.read(address, out);
}
bool write_u32(ImportCall& c, std::uint32_t address, std::uint32_t v) {
    std::array<std::byte, 4> b{};
    for (unsigned i = 0; i < 4; ++i) b[i] = std::byte((v >> (8 * i)) & 255);
    return c.runtime().memory.write(address, b);
}
}

struct GlesBinder {
    static void install(Gles2& g, ImportDispatcher& d) {
        const auto simple = [&](const char* name, GlOp op, unsigned argc, unsigned float_mask = 0) {
            d.bind(name, [&g, op, argc, float_mask](ImportCall& c) -> Err {
                GlCall call;
                call.op = op;
                unsigned fi = 0;
                for (unsigned i = 0; i < argc; ++i) {
                    if (float_mask & (1U << i)) call.f[fi++] = as_float(c.arg(i));
                    else call.a[i - fi] = c.arg(i);
                }
                c.set_result(g.backend_.execute(call).value);
                return std::nullopt;
            });
        };
        simple("glActiveTexture", GlOp::active_texture, 1);
        simple("glAttachShader", GlOp::attach_shader, 2);
        simple("glBlendFunc", GlOp::blend_func, 2);
        simple("glClear", GlOp::clear, 1);
        simple("glClearColor", GlOp::clear_color, 4, 0xF);
        simple("glClearDepthf", GlOp::clear_depthf, 1, 1);
        simple("glCompileShader", GlOp::compile_shader, 1);
        simple("glCullFace", GlOp::cull_face, 1);
        simple("glDepthFunc", GlOp::depth_func, 1);
        simple("glDepthMask", GlOp::depth_mask, 1);
        simple("glDisable", GlOp::disable, 1);
        simple("glEnable", GlOp::enable, 1);
        for (const bool on : {false, true}) {
            d.bind(on ? "glEnableVertexAttribArray" : "glDisableVertexAttribArray", [&g, on](ImportCall& c) -> Err {
                if (c.arg(0) >= g.attribs_.size()) return "vertex attribute index out of range";
                g.attribs_[c.arg(0)].enabled = on;
                GlCall call;
                call.op = on ? GlOp::enable_vertex_attrib_array : GlOp::disable_vertex_attrib_array;
                call.a[0] = c.arg(0);
                g.backend_.execute(call);
                return std::nullopt;
            });
        }
        simple("glFinish", GlOp::finish, 0);
        simple("glFrontFace", GlOp::front_face, 1);
        simple("glGetError", GlOp::get_error, 0);
        simple("glIsTexture", GlOp::is_texture, 1);
        simple("glLineWidth", GlOp::line_width, 1, 1);
        simple("glLinkProgram", GlOp::link_program, 1);
        simple("glPixelStorei", GlOp::pixel_store_i, 2);
        simple("glTexParameterf", GlOp::tex_parameter_f, 3, 4);
        simple("glTexParameteri", GlOp::tex_parameter_i, 3);
        simple("glUniform1f", GlOp::uniform_1f, 2, 2);
        simple("glUniform1i", GlOp::uniform_1i, 2);
        simple("glUseProgram", GlOp::use_program, 1);
        simple("glViewport", GlOp::viewport, 4);
        simple("glDeleteProgram", GlOp::delete_program, 1);
        simple("glDeleteShader", GlOp::delete_shader, 1);

        // objects whose ids the service allocates
        const auto create = [&](const char* name, GlOp op, unsigned argc) {
            d.bind(name, [&g, op, argc](ImportCall& c) -> Err {
                GlCall call;
                call.op = op;
                for (unsigned i = 0; i < argc; ++i) call.a[i] = c.arg(i);
                call.id = g.next_id_++;
                g.backend_.execute(call);
                c.set_result(call.id);
                return std::nullopt;
            });
        };
        create("glCreateProgram", GlOp::create_program, 0);
        create("glCreateShader", GlOp::create_shader, 1);
        const auto gen = [&](const char* name, GlOp op) {
            d.bind(name, [&g, op, name](ImportCall& c) -> Err {
                const auto n = c.arg(0);
                if (n > 4096) return std::string(name) + " count too large";
                for (std::uint32_t i = 0; i < n; ++i) {
                    GlCall call;
                    call.op = op;
                    call.id = g.next_id_++;
                    g.backend_.execute(call);
                    if (!write_u32(c, c.arg(1) + 4 * i, call.id)) return std::string(name) + " output is unwritable";
                }
                return std::nullopt;
            });
        };
        gen("glGenBuffers", GlOp::gen_buffer);
        gen("glGenTextures", GlOp::gen_texture);
        const auto del = [&](const char* name, GlOp op, bool buffer) {
            d.bind(name, [&g, op, name, buffer](ImportCall& c) -> Err {
                const auto n = c.arg(0);
                if (n > 4096) return std::string(name) + " count too large";
                for (std::uint32_t i = 0; i < n; ++i) {
                    std::array<std::byte, 4> b{};
                    if (!c.runtime().memory.read(c.arg(1) + 4 * i, b)) return std::string(name) + " input is unreadable";
                    std::uint32_t id = 0;
                    for (unsigned k = 0; k < 4; ++k) id |= std::uint32_t(b[k]) << (8 * k);
                    GlCall call;
                    call.op = op;
                    call.id = id;
                    if (buffer) {
                        if (g.array_buffer_ == id) g.array_buffer_ = 0;
                        if (g.element_buffer_ == id) g.element_buffer_ = 0;
                    }
                    g.backend_.execute(call);
                }
                return std::nullopt;
            });
        };
        del("glDeleteBuffers", GlOp::delete_buffer, true);
        del("glDeleteTextures", GlOp::delete_texture, false);

        d.bind("glBindBuffer", [&g](ImportCall& c) -> Err {
            if (c.arg(0) == array_buffer) g.array_buffer_ = c.arg(1);
            else if (c.arg(0) == element_buffer) g.element_buffer_ = c.arg(1);
            GlCall call;
            call.op = GlOp::bind_buffer;
            call.a = {c.arg(0), c.arg(1)};
            g.backend_.execute(call);
            return std::nullopt;
        });
        d.bind("glBindTexture", [&g](ImportCall& c) -> Err {
            GlCall call;
            call.op = GlOp::bind_texture;
            call.a = {c.arg(0), c.arg(1)};
            g.backend_.execute(call);
            return std::nullopt;
        });
        d.bind("glBufferData", [&g](ImportCall& c) -> Err {
            GlCall call;
            call.op = GlOp::buffer_data;
            call.a = {c.arg(0), c.arg(1), 0, c.arg(3)};  // target, size, (unused), usage
            if (c.arg(2) != 0 && !read_bytes(c, c.arg(2), c.arg(1), call.data)) return "glBufferData data is unreadable or too large";
            if (c.arg(2) == 0 && c.arg(1) > max_payload) return "glBufferData size too large";
            g.backend_.execute(call);
            return std::nullopt;
        });
        d.bind("glCompressedTexImage2D", [&g](ImportCall& c) -> Err {
            std::array<std::uint32_t, 10> v{};
            if (!load_args(c, 8, v)) return "glCompressedTexImage2D arguments are unreadable";
            GlCall call;
            call.op = GlOp::compressed_tex_image_2d;
            for (unsigned i = 0; i < 7; ++i) call.a[i] = c.arg(i);  // target level fmt w h border imageSize
            if (v[7] != 0 && !read_bytes(c, v[7], v[6], call.data)) return "glCompressedTexImage2D data is unreadable or too large";
            g.backend_.execute(call);
            return std::nullopt;
        });
        d.bind("glTexImage2D", [&g](ImportCall& c) -> Err {
            std::array<std::uint32_t, 10> v{};
            if (!load_args(c, 9, v)) return "glTexImage2D arguments are unreadable";
            GlCall call;
            call.op = GlOp::tex_image_2d;
            for (unsigned i = 0; i < 8; ++i) call.a[i] = c.arg(i);  // target level internal w h border format type
            if (v[8] != 0) {
                std::uint64_t channels = 0;
                switch (v[6]) { case 0x1908: channels = 4; break; case 0x1907: channels = 3; break;
                                    case 0x190A: channels = 2; break; case 0x1906: case 0x1909: channels = 1; break; default: break; }
                std::uint64_t bytes = channels;
                switch (v[7]) { case ubyte: break; case 0x8363: case 0x8033: case 0x8034: case ushort: bytes = 2; break;
                                    default: return "glTexImage2D pixel type is not supported"; }
                if (v[7] == ubyte) bytes = channels;
                if (bytes == 0) return "glTexImage2D format is not supported";
                if (!read_bytes(c, v[8], bytes * v[3] * v[4], call.data)) return "glTexImage2D pixels are unreadable or too large";
            }
            g.backend_.execute(call);
            return std::nullopt;
        });
        d.bind("glShaderSource", [&g](ImportCall& c) -> Err {
            GlCall call;
            call.op = GlOp::shader_source;
            call.a[0] = c.arg(0);
            const auto count = c.arg(1);
            if (count > 64) return "glShaderSource count too large";
            for (std::uint32_t i = 0; i < count; ++i) {
                std::array<std::byte, 4> b{};
                if (!c.runtime().memory.read(c.arg(2) + 4 * i, b)) return "glShaderSource string table is unreadable";
                std::uint32_t p = 0;
                for (unsigned k = 0; k < 4; ++k) p |= std::uint32_t(b[k]) << (8 * k);
                std::string part;
                if (c.arg(3) != 0) {
                    std::array<std::byte, 4> lb{};
                    if (!c.runtime().memory.read(c.arg(3) + 4 * i, lb)) return "glShaderSource length table is unreadable";
                    std::uint32_t len = 0;
                    for (unsigned k = 0; k < 4; ++k) len |= std::uint32_t(lb[k]) << (8 * k);
                    if (static_cast<std::int32_t>(len) >= 0) {
                        std::vector<std::byte> raw;
                        if (len > (1U << 20) || !read_bytes(c, p, len, raw)) return "glShaderSource text is unreadable";
                        part.assign(reinterpret_cast<const char*>(raw.data()), raw.size());
                        call.text += part;
                        continue;
                    }
                }
                if (!read_guest_string(c.runtime().memory, p, 1U << 20, part)) return "glShaderSource text is unreadable";
                call.text += part;
            }
            g.backend_.execute(call);
            return std::nullopt;
        });
        const auto locate = [&](const char* name, GlOp op) {
            d.bind(name, [&g, op, name](ImportCall& c) -> Err {
                GlCall call;
                call.op = op;
                call.a[0] = c.arg(0);
                if (!read_guest_string(c.runtime().memory, c.arg(1), 256, call.text)) return std::string(name) + " name is unreadable";
                c.set_result(g.backend_.execute(call).value);
                return std::nullopt;
            });
        };
        locate("glGetAttribLocation", GlOp::get_attrib_location);
        locate("glGetUniformLocation", GlOp::get_uniform_location);
        const auto iv = [&](const char* name, GlOp op) {
            d.bind(name, [&g, op, name](ImportCall& c) -> Err {
                GlCall call;
                call.op = op;
                call.a = {c.arg(0), c.arg(1)};
                if (!write_u32(c, c.arg(2), g.backend_.execute(call).value)) return std::string(name) + " output is unwritable";
                return std::nullopt;
            });
        };
        iv("glGetProgramiv", GlOp::get_program_iv);
        iv("glGetShaderiv", GlOp::get_shader_iv);
        d.bind("glGetShaderInfoLog", [&g](ImportCall& c) -> Err {
            GlCall call;
            call.op = GlOp::get_shader_info_log;
            call.a[0] = c.arg(0);
            auto text = g.backend_.execute(call).text;
            const auto cap = c.arg(1);
            if (cap == 0) return std::nullopt;
            if (text.size() > cap - 1) text.resize(cap - 1);
            std::vector<std::byte> out(text.size() + 1);
            std::memcpy(out.data(), text.data(), text.size());
            if (!c.runtime().memory.write(c.arg(3), out)) return "glGetShaderInfoLog buffer is unwritable";
            if (c.arg(2) != 0 && !write_u32(c, c.arg(2), static_cast<std::uint32_t>(text.size()))) return "glGetShaderInfoLog length is unwritable";
            return std::nullopt;
        });
        d.bind("glGetString", [&g](ImportCall& c) -> Err {
            if (g.config_.string_area == 0) return "glGetString has no scratch area";
            GlCall call;
            call.op = GlOp::get_string;
            call.a[0] = c.arg(0);
            auto text = g.backend_.execute(call).text;
            if (text.size() > 500) text.resize(500);
            std::vector<std::byte> out(text.size() + 1);
            std::memcpy(out.data(), text.data(), text.size());
            if (!c.runtime().memory.write(g.config_.string_area, out)) return "glGetString scratch area is unwritable";
            c.set_result(g.config_.string_area);
            return std::nullopt;
        });
        const auto uniform_array = [&](const char* name, GlOp op, std::uint32_t floats_per_item, bool matrix) {
            d.bind(name, [&g, op, floats_per_item, matrix, name](ImportCall& c) -> Err {
                GlCall call;
                call.op = op;
                call.a[0] = c.arg(0);
                call.a[1] = c.arg(1);
                if (matrix) call.a[2] = c.arg(2);
                const auto ptr = c.arg(matrix ? 3 : 2);
                if (c.arg(1) > 1024) return std::string(name) + " count too large";
                if (!read_bytes(c, ptr, std::uint64_t{c.arg(1)} * floats_per_item * 4, call.data)) return std::string(name) + " data is unreadable";
                g.backend_.execute(call);
                return std::nullopt;
            });
        };
        uniform_array("glUniform2fv", GlOp::uniform_2fv, 2, false);
        uniform_array("glUniform4fv", GlOp::uniform_4fv, 4, false);
        uniform_array("glUniformMatrix4fv", GlOp::uniform_matrix_4fv, 16, true);
        d.bind("glReadPixels", [&g](ImportCall& c) -> Err {
            std::array<std::uint32_t, 10> v{};
            if (!load_args(c, 7, v)) return "glReadPixels arguments are unreadable";
            if (v[4] != 0x1908 || v[5] != ubyte) return "glReadPixels supports RGBA/UNSIGNED_BYTE only";
            GlCall call;
            call.op = GlOp::read_pixels;
            for (unsigned i = 0; i < 6; ++i) call.a[i] = c.arg(i);
            const std::uint64_t size = std::uint64_t{v[2]} * v[3] * 4;
            if (size > max_payload) return "glReadPixels area too large";
            auto r = g.backend_.execute(call);
            r.bytes.resize(static_cast<std::size_t>(size));
            if (!c.runtime().memory.write(v[6], r.bytes)) return "glReadPixels buffer is unwritable";
            return std::nullopt;
        });
        d.bind("glVertexAttribPointer", [&g](ImportCall& c) -> Err {
            std::array<std::uint32_t, 10> v{};
            if (!load_args(c, 6, v)) return "glVertexAttribPointer arguments are unreadable";
            if (v[0] >= g.attribs_.size()) return "glVertexAttribPointer index out of range";
            auto& a = g.attribs_[v[0]];
            a.size = v[1]; a.type = v[2]; a.normalized = v[3] != 0; a.stride = v[4];
            a.pointer = v[5]; a.buffer = g.array_buffer_;
            GlCall call;
            call.op = GlOp::vertex_attrib_pointer;
            call.a = {v[0], v[1], v[2], v[3], v[4], v[5], g.array_buffer_};
            g.backend_.execute(call);
            return std::nullopt;
        });
        d.bind("glDrawArrays", [&g](ImportCall& c) -> Err {
            GlCall call;
            call.op = GlOp::draw_arrays;
            call.a = {c.arg(0), c.arg(1), c.arg(2)};
            if (c.arg(2) > (1U << 24) || c.arg(1) > (1U << 24)) return "glDrawArrays range too large";
            if (auto e = g.gather(c, call, c.arg(1) + c.arg(2))) return e;
            g.backend_.execute(call);
            return std::nullopt;
        });
        d.bind("glDrawElements", [&g](ImportCall& c) -> Err {
            GlCall call;
            call.op = GlOp::draw_elements;
            call.a = {c.arg(0), c.arg(1), c.arg(2), c.arg(3), g.element_buffer_};
            const auto size = type_size(c.arg(2));
            if (c.arg(2) != ubyte && c.arg(2) != ushort && c.arg(2) != uint_) return "glDrawElements index type is not supported";
            if (c.arg(1) > (1U << 24)) return "glDrawElements count too large";
            std::uint32_t max_index = 0;
            if (g.element_buffer_ == 0) {
                if (!read_bytes(c, c.arg(3), std::uint64_t{c.arg(1)} * size, call.data)) return "glDrawElements indices are unreadable";
                for (std::uint32_t i = 0; i < c.arg(1); ++i) {
                    std::uint32_t v = 0;
                    for (std::uint32_t k = 0; k < size; ++k) v |= std::uint32_t(call.data[i * size + k]) << (8 * k);
                    max_index = std::max(max_index, v);
                }
                if (auto e = g.gather(c, call, max_index + 1)) return e;
            } else if (auto e = g.gather(c, call, 0)) {
                return e;
            }
            g.backend_.execute(call);
            return std::nullopt;
        });
        d.bind("eglSwapBuffers", [&g](ImportCall& c) -> Err {
            GlCall call;
            call.op = GlOp::swap_buffers;
            g.backend_.execute(call);
            c.set_result(1);
            return std::nullopt;
        });
        d.bind("eglGetProcAddress", [](ImportCall&) -> Err {
            return "eglGetProcAddress is not supported yet (extension lookup needs a per-name trap)";
        });
    }
};

void Gles2::install(ImportDispatcher& dispatcher) { GlesBinder::install(*this, dispatcher); }

std::optional<std::string> Gles2::gather(ImportCall& c, GlCall& out, std::uint32_t vertex_count) {
    out.vertex_count = vertex_count;
    if (vertex_count == 0) return std::nullopt;
    for (std::uint32_t i = 0; i < attribs_.size(); ++i) {
        const auto& a = attribs_[i];
        if (!a.enabled || a.buffer != 0) continue;
        const auto element = a.size * type_size(a.type);
        if (element == 0) return "client-side vertex attribute has an unsupported size or type";
        const auto stride = a.stride != 0 ? a.stride : element;
        GlAttribData data;
        data.index = i; data.size = a.size; data.type = a.type; data.stride = a.stride; data.normalized = a.normalized;
        if (!read_bytes(c, a.pointer, std::uint64_t{vertex_count - 1} * stride + element, data.bytes))
            return "client-side vertex attribute data is unreadable or too large";
        out.attribs.push_back(std::move(data));
    }
    return std::nullopt;
}
}
