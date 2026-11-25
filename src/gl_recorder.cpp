// SPDX-License-Identifier: GPL-2.0-or-later
#include "atlan/gl_recorder.hpp"

namespace atlan {
const char* gl_op_name(GlOp op) {
    switch (op) {
        case GlOp::active_texture: return "active_texture";
        case GlOp::attach_shader: return "attach_shader";
        case GlOp::bind_buffer: return "bind_buffer";
        case GlOp::bind_texture: return "bind_texture";
        case GlOp::blend_func: return "blend_func";
        case GlOp::buffer_data: return "buffer_data";
        case GlOp::clear: return "clear";
        case GlOp::clear_color: return "clear_color";
        case GlOp::clear_depthf: return "clear_depthf";
        case GlOp::compile_shader: return "compile_shader";
        case GlOp::compressed_tex_image_2d: return "compressed_tex_image_2d";
        case GlOp::create_program: return "create_program";
        case GlOp::create_shader: return "create_shader";
        case GlOp::cull_face: return "cull_face";
        case GlOp::delete_buffer: return "delete_buffer";
        case GlOp::delete_program: return "delete_program";
        case GlOp::delete_shader: return "delete_shader";
        case GlOp::delete_texture: return "delete_texture";
        case GlOp::depth_func: return "depth_func";
        case GlOp::depth_mask: return "depth_mask";
        case GlOp::disable: return "disable";
        case GlOp::disable_vertex_attrib_array: return "disable_vertex_attrib_array";
        case GlOp::draw_arrays: return "draw_arrays";
        case GlOp::draw_elements: return "draw_elements";
        case GlOp::enable: return "enable";
        case GlOp::enable_vertex_attrib_array: return "enable_vertex_attrib_array";
        case GlOp::finish: return "finish";
        case GlOp::front_face: return "front_face";
        case GlOp::gen_buffer: return "gen_buffer";
        case GlOp::gen_texture: return "gen_texture";
        case GlOp::get_attrib_location: return "get_attrib_location";
        case GlOp::get_error: return "get_error";
        case GlOp::get_program_iv: return "get_program_iv";
        case GlOp::get_shader_info_log: return "get_shader_info_log";
        case GlOp::get_shader_iv: return "get_shader_iv";
        case GlOp::get_string: return "get_string";
        case GlOp::get_uniform_location: return "get_uniform_location";
        case GlOp::is_texture: return "is_texture";
        case GlOp::line_width: return "line_width";
        case GlOp::link_program: return "link_program";
        case GlOp::pixel_store_i: return "pixel_store_i";
        case GlOp::read_pixels: return "read_pixels";
        case GlOp::shader_source: return "shader_source";
        case GlOp::tex_image_2d: return "tex_image_2d";
        case GlOp::tex_parameter_f: return "tex_parameter_f";
        case GlOp::tex_parameter_i: return "tex_parameter_i";
        case GlOp::uniform_1f: return "uniform_1f";
        case GlOp::uniform_1i: return "uniform_1i";
        case GlOp::uniform_2fv: return "uniform_2fv";
        case GlOp::uniform_4fv: return "uniform_4fv";
        case GlOp::uniform_matrix_4fv: return "uniform_matrix_4fv";
        case GlOp::use_program: return "use_program";
        case GlOp::vertex_attrib_pointer: return "vertex_attrib_pointer";
        case GlOp::viewport: return "viewport";
        case GlOp::swap_buffers: return "swap_buffers";
    }
    return "?";
}

GlResult RecordingGlBackend::execute(const GlCall& c) {
    GlResult r;
    const char* name = gl_op_name(c.op);
    ++counts[name];
    std::fprintf(out_, "%s id=%u a=[%u,%u,%u,%u,%u,%u,%u,%u] f=[%g,%g,%g,%g] data=%zu vcount=%u", name, c.id, c.a[0], c.a[1], c.a[2], c.a[3],
                 c.a[4], c.a[5], c.a[6], c.a[7], double(c.f[0]), double(c.f[1]), double(c.f[2]), double(c.f[3]), c.data.size(), c.vertex_count);
    if (!c.text.empty()) {
        if (c.op == GlOp::shader_source && !show_sources_) std::fprintf(out_, " text=%zu bytes", c.text.size());
        else std::fprintf(out_, " text=\"%s\"", c.text.c_str());
    }
    for (const auto& a : c.attribs)
        std::fprintf(out_, " attrib{%u size=%u type=0x%x stride=%u norm=%d bytes=%zu}", a.index, a.size, a.type, a.stride, int(a.normalized), a.bytes.size());
    std::fputc('\n', out_);
    if (c.op == GlOp::get_program_iv || c.op == GlOp::get_shader_iv) r.value = 1;
    if (c.op == GlOp::get_attrib_location || c.op == GlOp::get_uniform_location) r.value = 0;
    if (c.op == GlOp::get_string) r.text = "atlan recorder";
    return r;
}

void LoggingGlSink::clear(const GlClear& c) {
    std::fprintf(out_, "sink clear color=%d depth=%d rgba=[%g,%g,%g,%g] z=%g\n", int(c.color), int(c.depth), double(c.rgba[0]), double(c.rgba[1]),
                 double(c.rgba[2]), double(c.rgba[3]), double(c.depth_value));
}
void LoggingGlSink::draw(const GlDraw& d) {
    ++draws;
    std::fprintf(out_, "sink draw program=%llu prim=%d indices=%zu vertices=%u blend=%d(%x,%x) cull=%d depth=%d vp=[%d,%d,%d,%d] tex=[",
                 static_cast<unsigned long long>(d.program->serial), int(d.primitive), d.indices.size(), d.vertex_count, int(d.state.blend),
                 d.state.blend_src, d.state.blend_dst, int(d.state.cull), int(d.state.depth_test), d.state.viewport[0], d.state.viewport[1],
                 d.state.viewport[2], d.state.viewport[3]);
    for (const auto& t : d.textures) {
        if (t.image) std::fprintf(out_, "%ux%u ", t.image->width, t.image->height);
        else std::fprintf(out_, "none ");
    }
    std::fprintf(out_, "]\n");
}
void LoggingGlSink::present() {
    ++presents;
    std::fprintf(out_, "sink present\n");
}
}
