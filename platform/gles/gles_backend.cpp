// SPDX-License-Identifier: GPL-2.0-or-later
#include "gles_backend.hpp"
#include <EGL/egl.h>
#include <GLES2/gl2.h>
#include <GLES2/gl2ext.h>
#include <array>
#include <cstring>
#include <mutex>
#include <unordered_map>
#include <unordered_set>

namespace atlan {
namespace {
using FramebufferTexture2DMultisample = void (*)(GLenum, GLenum, GLenum, GLuint, GLint, GLsizei);
using RenderbufferStorageMultisample = void (*)(GLenum, GLsizei, GLenum, GLsizei, GLsizei);
constexpr int max_attribs = 16;
GLint as_int(std::uint32_t v) { return static_cast<GLint>(v); }
const void* offset(std::uint32_t v) { return reinterpret_cast<const void*>(static_cast<std::uintptr_t>(v)); }
bool has_extension(const char* list, const char* name) {
    if (!list) return false;
    const std::size_t n = std::strlen(name);
    for (const char* p = list; (p = std::strstr(p, name)) != nullptr; p += n)
        if ((p == list || p[-1] == ' ') && (p[n] == ' ' || p[n] == 0)) return true;
    return false;
}
}

struct GlesBackend::Impl {
    GlesHost& host;
    std::function<void(const std::string&)> log;
    std::mutex stats_lock;
    Stats stats;
    bool attached{}, failed{};
    // guest id -> driver name
    std::unordered_map<std::uint32_t, GLuint> shaders, programs, buffers, textures;
    std::unordered_set<std::string> reported;
    // what the guest has bound / enabled, for client-side arrays
    std::uint32_t array_buffer{};
    std::array<bool, max_attribs> enabled{}, client{};
    // the guest's default framebuffer
    GLuint framebuffer{}, color{}, depth{};
    std::uint32_t width{}, height{};
    bool multisample{};
    FramebufferTexture2DMultisample texture_multisample{};
    RenderbufferStorageMultisample storage_multisample{};
    bool depth24{};

    explicit Impl(GlesHost& h) : host(h) {}
    void note(const std::string& line) { if (log) log(line); }
    void once(const std::string& key, const std::string& line) {
        if (reported.insert(key).second) note(line);
    }
    GLuint name(std::unordered_map<std::uint32_t, GLuint>& map, std::uint32_t id) const {
        if (id == 0) return 0;
        const auto it = map.find(id);
        return it == map.end() ? 0 : it->second;
    }
    // GLES2 creates an object on its first bind; ids the guest made up (never generated) get a driver name then.
    GLuint bound_name(std::unordered_map<std::uint32_t, GLuint>& map, std::uint32_t id, bool texture) {
        if (id == 0) return 0;
        if (const auto it = map.find(id); it != map.end()) return it->second;
        GLuint n = 0;
        if (texture) glGenTextures(1, &n); else glGenBuffers(1, &n);
        map[id] = n;
        return n;
    }

    bool attach() {
        if (attached) return !failed;
        attached = true;
        std::string error;
        if (!host.attach(error)) {
            failed = true;
            note("gles unavailable: " + error);
            return false;
        }
        const char* extensions = reinterpret_cast<const char*>(glGetString(GL_EXTENSIONS));
        depth24 = has_extension(extensions, "GL_OES_depth24");
        if (multisample && has_extension(extensions, "GL_EXT_multisampled_render_to_texture")) {
            texture_multisample = reinterpret_cast<FramebufferTexture2DMultisample>(eglGetProcAddress("glFramebufferTexture2DMultisampleEXT"));
            storage_multisample = reinterpret_cast<RenderbufferStorageMultisample>(eglGetProcAddress("glRenderbufferStorageMultisampleEXT"));
        }
        const auto text = [](GLenum e) { const auto* s = glGetString(e); return std::string(s ? reinterpret_cast<const char*>(s) : "?"); };
        note("gles " + text(GL_VERSION) + " / " + text(GL_RENDERER));
        return true;
    }

    void release_framebuffer() {
        if (framebuffer) glDeleteFramebuffers(1, &framebuffer);
        if (color) glDeleteTextures(1, &color);
        if (depth) glDeleteRenderbuffers(1, &depth);
        framebuffer = color = depth = 0;
    }
    // (Re)creates the guest's default framebuffer at w x h and leaves it bound. Texture unit and binding state the
    // guest relies on are restored.
    void make_framebuffer(std::uint32_t w, std::uint32_t h) {
        if (w == 0 || h == 0) return;
        if (framebuffer && w == width && h == height) return;
        GLint active = 0, bound_texture = 0, bound_renderbuffer = 0;
        glGetIntegerv(GL_ACTIVE_TEXTURE, &active);
        glActiveTexture(GL_TEXTURE0);
        glGetIntegerv(GL_TEXTURE_BINDING_2D, &bound_texture);
        glGetIntegerv(GL_RENDERBUFFER_BINDING, &bound_renderbuffer);
        release_framebuffer();
        width = w;
        height = h;
        glGenTextures(1, &color);
        glBindTexture(GL_TEXTURE_2D, color);
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, as_int(w), as_int(h), 0, GL_RGBA, GL_UNSIGNED_BYTE, nullptr);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
        glGenFramebuffers(1, &framebuffer);
        glBindFramebuffer(GL_FRAMEBUFFER, framebuffer);
        glGenRenderbuffers(1, &depth);
        glBindRenderbuffer(GL_RENDERBUFFER, depth);
        const GLenum depth_format = depth24 ? GL_DEPTH_COMPONENT24_OES : GL_DEPTH_COMPONENT16;
        bool sampled = false;
        if (texture_multisample && storage_multisample) {
            storage_multisample(GL_RENDERBUFFER, 4, depth_format, as_int(w), as_int(h));
            texture_multisample(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, color, 0, 4);
            glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_DEPTH_ATTACHMENT, GL_RENDERBUFFER, depth);
            sampled = glCheckFramebufferStatus(GL_FRAMEBUFFER) == GL_FRAMEBUFFER_COMPLETE;
            if (!sampled) {  // fall back to a plain framebuffer
                glDeleteRenderbuffers(1, &depth);
                glGenRenderbuffers(1, &depth);
                glBindRenderbuffer(GL_RENDERBUFFER, depth);
            }
        }
        if (!sampled) {
            glRenderbufferStorage(GL_RENDERBUFFER, depth_format, as_int(w), as_int(h));
            glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, color, 0);
            glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_DEPTH_ATTACHMENT, GL_RENDERBUFFER, depth);
        }
        const GLenum status = glCheckFramebufferStatus(GL_FRAMEBUFFER);
        glBindTexture(GL_TEXTURE_2D, static_cast<GLuint>(bound_texture));
        glBindRenderbuffer(GL_RENDERBUFFER, static_cast<GLuint>(bound_renderbuffer));
        glActiveTexture(static_cast<GLenum>(active));
        note("gles framebuffer " + std::to_string(w) + "x" + std::to_string(h) + (sampled ? " 4x" : "") +
             (status == GL_FRAMEBUFFER_COMPLETE ? "" : " incomplete " + std::to_string(status)));
    }

    // Points the enabled client-side attributes at this call's gathered bytes. False: an enabled client attribute
    // has no data for this draw (the driver would read a stale host pointer), so the draw must be skipped.
    bool bind_client_arrays(const GlCall& call) {
        std::array<bool, max_attribs> given{};
        bool unbound = false;
        for (const auto& a : call.attribs) {
            if (a.index >= max_attribs) continue;
            if (!unbound) { glBindBuffer(GL_ARRAY_BUFFER, 0); unbound = true; }
            glVertexAttribPointer(a.index, as_int(a.size), a.type, a.normalized ? GL_TRUE : GL_FALSE, as_int(a.stride), a.bytes.data());
            given[a.index] = true;
        }
        if (unbound) glBindBuffer(GL_ARRAY_BUFFER, name(buffers, array_buffer));
        for (int i = 0; i < max_attribs; ++i)
            if (enabled[i] && client[i] && !given[i]) return false;
        return true;
    }
    void count_draw(bool drawn) {
        std::lock_guard lock(stats_lock);
        if (drawn) ++stats.draws; else ++stats.draws_skipped;
    }
};

GlesBackend::GlesBackend(GlesHost& host) : impl_(std::make_unique<Impl>(host)) {}
GlesBackend::~GlesBackend() = default;

void GlesBackend::set_log(std::function<void(const std::string&)> log) { impl_->log = std::move(log); }

void GlesBackend::set_surface_size(std::uint32_t width, std::uint32_t height) noexcept {
    if (width == 0 || height == 0) return;
    pending_size_.store((std::uint64_t{width} << 32) | height, std::memory_order_release);
}

GlesBackend::Stats GlesBackend::stats() const noexcept {
    std::lock_guard lock(impl_->stats_lock);
    return impl_->stats;
}

GlResult GlesBackend::execute(const GlCall& c) {
    auto& s = *impl_;
    GlResult r;
    if (!s.attached) s.multisample = multisample_;
    if (!s.attach()) {
        // No context: answer like the null backend so the guest carries on.
        if (c.op == GlOp::get_program_iv || c.op == GlOp::get_shader_iv) r.value = 1;
        return r;
    }
    if (const auto size = pending_size_.exchange(0, std::memory_order_acq_rel); size != 0)
        s.make_framebuffer(static_cast<std::uint32_t>(size >> 32), static_cast<std::uint32_t>(size));
    const auto& a = c.a;
    switch (c.op) {
        case GlOp::active_texture: glActiveTexture(a[0]); break;
        case GlOp::attach_shader: glAttachShader(s.name(s.programs, a[0]), s.name(s.shaders, a[1])); break;
        case GlOp::bind_buffer:
            if (a[0] == GL_ARRAY_BUFFER) s.array_buffer = a[1];
            glBindBuffer(a[0], s.bound_name(s.buffers, a[1], false));
            break;
        case GlOp::bind_texture: glBindTexture(a[0], s.bound_name(s.textures, a[1], true)); break;
        case GlOp::blend_func: glBlendFunc(a[0], a[1]); break;
        case GlOp::buffer_data:
            glBufferData(a[0], static_cast<GLsizeiptr>(a[1]), c.data.empty() ? nullptr : c.data.data(), a[3]);
            break;
        case GlOp::clear: glClear(a[0]); break;
        case GlOp::clear_color: glClearColor(c.f[0], c.f[1], c.f[2], c.f[3]); break;
        case GlOp::clear_depthf: glClearDepthf(c.f[0]); break;
        case GlOp::compile_shader: glCompileShader(s.name(s.shaders, a[0])); break;
        case GlOp::compressed_tex_image_2d:
            glCompressedTexImage2D(a[0], as_int(a[1]), a[2], as_int(a[3]), as_int(a[4]), as_int(a[5]), as_int(a[6]),
                                   c.data.empty() ? nullptr : c.data.data());
            break;
        case GlOp::create_program: s.programs[c.id] = glCreateProgram(); break;
        case GlOp::create_shader: s.shaders[c.id] = glCreateShader(a[0]); break;
        case GlOp::cull_face: glCullFace(a[0]); break;
        case GlOp::delete_buffer:
            if (const auto n = s.name(s.buffers, c.id)) { glDeleteBuffers(1, &n); s.buffers.erase(c.id); }
            if (s.array_buffer == c.id) s.array_buffer = 0;
            break;
        case GlOp::delete_texture:
            if (const auto n = s.name(s.textures, c.id)) { glDeleteTextures(1, &n); s.textures.erase(c.id); }
            break;
        case GlOp::delete_program:
            if (const auto n = s.name(s.programs, a[0])) { glDeleteProgram(n); s.programs.erase(a[0]); }
            break;
        case GlOp::delete_shader:
            if (const auto n = s.name(s.shaders, a[0])) { glDeleteShader(n); s.shaders.erase(a[0]); }
            break;
        case GlOp::depth_func: glDepthFunc(a[0]); break;
        case GlOp::depth_mask: glDepthMask(a[0] ? GL_TRUE : GL_FALSE); break;
        case GlOp::disable: glDisable(a[0]); break;
        case GlOp::enable: glEnable(a[0]); break;
        case GlOp::disable_vertex_attrib_array:
        case GlOp::enable_vertex_attrib_array: {
            const bool on = c.op == GlOp::enable_vertex_attrib_array;
            if (a[0] < max_attribs) s.enabled[a[0]] = on;
            if (on) glEnableVertexAttribArray(a[0]); else glDisableVertexAttribArray(a[0]);
            break;
        }
        case GlOp::draw_arrays: {
            const bool ok = s.bind_client_arrays(c);
            if (ok) glDrawArrays(a[0], as_int(a[1]), as_int(a[2]));
            else s.once("draw_arrays", "gles draw skipped: client-side attribute without data");
            s.count_draw(ok);
            break;
        }
        case GlOp::draw_elements: {
            const bool ok = s.bind_client_arrays(c);
            if (ok) glDrawElements(a[0], as_int(a[1]), a[2], a[4] != 0 ? offset(a[3]) : c.data.data());
            else s.once("draw_elements", "gles draw skipped: client-side attribute with indices in a buffer");
            s.count_draw(ok);
            break;
        }
        case GlOp::finish: glFinish(); break;
        case GlOp::front_face: glFrontFace(a[0]); break;
        case GlOp::gen_buffer: { GLuint n = 0; glGenBuffers(1, &n); s.buffers[c.id] = n; break; }
        case GlOp::gen_texture: { GLuint n = 0; glGenTextures(1, &n); s.textures[c.id] = n; break; }
        case GlOp::get_attrib_location:
            r.value = static_cast<std::uint32_t>(glGetAttribLocation(s.name(s.programs, a[0]), c.text.c_str()));
            break;
        case GlOp::get_uniform_location:
            r.value = static_cast<std::uint32_t>(glGetUniformLocation(s.name(s.programs, a[0]), c.text.c_str()));
            break;
        case GlOp::get_error: r.value = glGetError(); break;
        case GlOp::get_program_iv: {
            GLint v = 0;
            glGetProgramiv(s.name(s.programs, a[0]), a[1], &v);
            r.value = static_cast<std::uint32_t>(v);
            if (a[1] == GL_LINK_STATUS && v == 0) {
                std::lock_guard lock(s.stats_lock);
                ++s.stats.link_failures;
            }
            break;
        }
        case GlOp::get_shader_iv: {
            GLint v = 0;
            glGetShaderiv(s.name(s.shaders, a[0]), a[1], &v);
            r.value = static_cast<std::uint32_t>(v);
            if (a[1] == GL_COMPILE_STATUS && v == 0) {
                std::lock_guard lock(s.stats_lock);
                ++s.stats.shader_failures;
            }
            break;
        }
        case GlOp::get_shader_info_log: {
            const GLuint n = s.name(s.shaders, a[0]);
            GLint length = 0;
            glGetShaderiv(n, GL_INFO_LOG_LENGTH, &length);
            if (length > 0) {
                std::string text(static_cast<std::size_t>(length), '\0');
                GLsizei written = 0;
                glGetShaderInfoLog(n, length, &written, text.data());
                text.resize(static_cast<std::size_t>(written));
                r.text = std::move(text);
            }
            break;
        }
        case GlOp::get_string: {
            const auto* text = glGetString(a[0]);
            if (text) r.text = reinterpret_cast<const char*>(text);
            break;
        }
        case GlOp::is_texture: {
            const auto n = s.name(s.textures, a[0]);
            r.value = n != 0 && glIsTexture(n) ? 1 : 0;
            break;
        }
        case GlOp::line_width: glLineWidth(c.f[0]); break;
        case GlOp::link_program: glLinkProgram(s.name(s.programs, a[0])); break;
        case GlOp::pixel_store_i: glPixelStorei(a[0], as_int(a[1])); break;
        case GlOp::read_pixels:
            r.bytes.resize(std::size_t{a[2]} * a[3] * 4);
            if (!r.bytes.empty()) glReadPixels(as_int(a[0]), as_int(a[1]), as_int(a[2]), as_int(a[3]), GL_RGBA, GL_UNSIGNED_BYTE, r.bytes.data());
            break;
        case GlOp::shader_source: {
            const GLchar* text = c.text.c_str();
            const GLint length = static_cast<GLint>(c.text.size());
            glShaderSource(s.name(s.shaders, a[0]), 1, &text, &length);
            break;
        }
        case GlOp::tex_image_2d:
            glTexImage2D(a[0], as_int(a[1]), as_int(a[2]), as_int(a[3]), as_int(a[4]), as_int(a[5]), a[6], a[7],
                         c.data.empty() ? nullptr : c.data.data());
            break;
        case GlOp::tex_parameter_f: glTexParameterf(a[0], a[1], c.f[0]); break;
        case GlOp::tex_parameter_i: glTexParameteri(a[0], a[1], as_int(a[2])); break;
        case GlOp::uniform_1f: glUniform1f(as_int(a[0]), c.f[0]); break;
        case GlOp::uniform_1i: glUniform1i(as_int(a[0]), as_int(a[1])); break;
        case GlOp::uniform_2fv:
            glUniform2fv(as_int(a[0]), as_int(a[1]), reinterpret_cast<const GLfloat*>(c.data.data()));
            break;
        case GlOp::uniform_4fv:
            glUniform4fv(as_int(a[0]), as_int(a[1]), reinterpret_cast<const GLfloat*>(c.data.data()));
            break;
        case GlOp::uniform_matrix_4fv:
            glUniformMatrix4fv(as_int(a[0]), as_int(a[1]), a[2] ? GL_TRUE : GL_FALSE, reinterpret_cast<const GLfloat*>(c.data.data()));
            break;
        case GlOp::use_program: glUseProgram(s.name(s.programs, a[0])); break;
        case GlOp::vertex_attrib_pointer:
            if (a[0] < max_attribs) s.client[a[0]] = a[6] == 0;
            // A client-side pointer is a guest address; it is set from the gathered bytes at each draw instead.
            if (a[6] != 0) glVertexAttribPointer(a[0], as_int(a[1]), a[2], a[3] ? GL_TRUE : GL_FALSE, as_int(a[4]), offset(a[5]));
            break;
        case GlOp::viewport: glViewport(as_int(a[0]), as_int(a[1]), as_int(a[2]), as_int(a[3])); break;
        case GlOp::swap_buffers:
            if (s.framebuffer) s.host.present(s.color, s.width, s.height);
            {
                std::lock_guard lock(s.stats_lock);
                ++s.stats.frames;
            }
            break;
    }
    return r;
}
}
