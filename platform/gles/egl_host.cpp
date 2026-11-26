// SPDX-License-Identifier: GPL-2.0-or-later
#include "egl_host.hpp"
#include <EGL/egl.h>
#include <EGL/eglext.h>
#include <GLES2/gl2.h>
#include <atomic>
#include <cstdio>
#include <cstring>
#include <mutex>
#if defined(__ANDROID__)
#include <android/native_window.h>
#endif

namespace atlan {
namespace {
#ifndef EGL_PLATFORM_SURFACELESS_MESA
#define EGL_PLATFORM_SURFACELESS_MESA 0x31DD
#endif
bool has_extension(const char* list, const char* name) {
    if (!list) return false;
    const std::size_t n = std::strlen(name);
    for (const char* p = list; (p = std::strstr(p, name)) != nullptr; p += n)
        if ((p == list || p[-1] == ' ') && (p[n] == ' ' || p[n] == 0)) return true;
    return false;
}
std::string egl_error(const char* what) {
    char code[16];
    std::snprintf(code, sizeof code, "0x%04x", static_cast<unsigned>(eglGetError()));
    return std::string(what) + " failed (" + code + ")";
}
constexpr const char* present_vertex =
    "attribute vec2 p;\nvarying vec2 t;\nvoid main() { t = p * 0.5 + 0.5; gl_Position = vec4(p, 0.0, 1.0); }\n";
constexpr const char* present_fragment =
    "precision mediump float;\nvarying vec2 t;\nuniform sampler2D s;\n"
    "void main() { gl_FragColor = vec4(texture2D(s, t).rgb, 1.0); }\n";
}

struct EglHost::Impl {
    std::function<void(const std::string&)> log;
    Capture capture;
    EGLDisplay display{EGL_NO_DISPLAY};
    EGLConfig config{};
    EGLContext guest{EGL_NO_CONTEXT}, shower{EGL_NO_CONTEXT};
    EGLSurface pbuffer{EGL_NO_SURFACE};
    std::mutex window_lock;  // guards window and window_surface
    ANativeWindow* window{};
    EGLSurface window_surface{EGL_NO_SURFACE};
    GLuint program{};
    GLint sampler{-1};
    std::atomic<std::uint64_t> shown{0};
    bool window_failed{};
    std::vector<std::uint8_t> pixels, row;

    void note(const std::string& line) { if (log) log(line); }

    bool make_guest_current() { return eglMakeCurrent(display, pbuffer, pbuffer, guest) == EGL_TRUE; }

    GLuint compile(GLenum type, const char* source) {
        const GLuint shader = glCreateShader(type);
        glShaderSource(shader, 1, &source, nullptr);
        glCompileShader(shader);
        return shader;
    }
    bool make_program() {
        if (program) return true;
        const GLuint vs = compile(GL_VERTEX_SHADER, present_vertex), fs = compile(GL_FRAGMENT_SHADER, present_fragment);
        program = glCreateProgram();
        glAttachShader(program, vs);
        glAttachShader(program, fs);
        glBindAttribLocation(program, 0, "p");
        glLinkProgram(program);
        glDeleteShader(vs);
        glDeleteShader(fs);
        GLint linked = 0;
        glGetProgramiv(program, GL_LINK_STATUS, &linked);
        if (!linked) {
            note("egl present program failed to link");
            glDeleteProgram(program);
            program = 0;
            return false;
        }
        sampler = glGetUniformLocation(program, "s");
        return true;
    }

    void drop_window_surface() {
        if (window_surface != EGL_NO_SURFACE) eglDestroySurface(display, window_surface);
        window_surface = EGL_NO_SURFACE;
    }

    // Shows `texture` on the window with the presenting context; returns with the guest context current again.
    void show(GLuint texture) {
#if defined(__ANDROID__)
        std::lock_guard lock(window_lock);
        if (!window) return;
        if (window_surface == EGL_NO_SURFACE) {
            window_surface = eglCreateWindowSurface(display, config, window, nullptr);
            if (window_surface == EGL_NO_SURFACE) {
                if (!window_failed) note(egl_error("eglCreateWindowSurface"));
                window_failed = true;
                return;
            }
            window_failed = false;
        }
        glFlush();  // the guest's frame must be complete before the other context samples it
        if (eglMakeCurrent(display, window_surface, window_surface, shower) != EGL_TRUE) {
            note(egl_error("eglMakeCurrent(window)"));
            drop_window_surface();
            make_guest_current();
            return;
        }
        EGLint width = 0, height = 0;
        eglQuerySurface(display, window_surface, EGL_WIDTH, &width);
        eglQuerySurface(display, window_surface, EGL_HEIGHT, &height);
        if (make_program()) {
            static constexpr GLfloat quad[] = {-1, -1, 1, -1, -1, 1, 1, 1};
            glViewport(0, 0, width, height);
            glUseProgram(program);
            glActiveTexture(GL_TEXTURE0);
            glBindTexture(GL_TEXTURE_2D, texture);
            glUniform1i(sampler, 0);
            glBindBuffer(GL_ARRAY_BUFFER, 0);
            glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 0, quad);
            glEnableVertexAttribArray(0);
            glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
            if (eglSwapBuffers(display, window_surface) == EGL_TRUE) shown.fetch_add(1, std::memory_order_relaxed);
            else note(egl_error("eglSwapBuffers"));
        }
        if (!make_guest_current()) note(egl_error("eglMakeCurrent(guest)"));
#else
        (void)texture;
#endif
    }
};

EglHost::EglHost() : impl_(std::make_unique<Impl>()) {}

EglHost::~EglHost() {
    auto& s = *impl_;
    if (s.display == EGL_NO_DISPLAY) return;
    {
        std::lock_guard lock(s.window_lock);
        s.drop_window_surface();
#if defined(__ANDROID__)
        if (s.window) ANativeWindow_release(s.window);
#endif
        s.window = nullptr;
    }
    // The contexts may still be current on the (finished) guest thread; EGL frees them once they are not.
    if (s.shower != EGL_NO_CONTEXT) eglDestroyContext(s.display, s.shower);
    if (s.guest != EGL_NO_CONTEXT) eglDestroyContext(s.display, s.guest);
    if (s.pbuffer != EGL_NO_SURFACE) eglDestroySurface(s.display, s.pbuffer);
}

void EglHost::set_log(std::function<void(const std::string&)> log) { impl_->log = std::move(log); }
void EglHost::set_capture(Capture capture) { impl_->capture = std::move(capture); }
std::uint64_t EglHost::frames_shown() const noexcept { return impl_->shown.load(std::memory_order_relaxed); }

bool EglHost::attach(std::string& error) {
    auto& s = *impl_;
#if !defined(__ANDROID__)
    if (has_extension(eglQueryString(EGL_NO_DISPLAY, EGL_EXTENSIONS), "EGL_MESA_platform_surfaceless")) {
        if (auto get = reinterpret_cast<PFNEGLGETPLATFORMDISPLAYEXTPROC>(eglGetProcAddress("eglGetPlatformDisplayEXT")))
            s.display = get(EGL_PLATFORM_SURFACELESS_MESA, EGL_DEFAULT_DISPLAY, nullptr);
    }
#endif
    if (s.display == EGL_NO_DISPLAY) s.display = eglGetDisplay(EGL_DEFAULT_DISPLAY);
    if (s.display == EGL_NO_DISPLAY) { error = egl_error("eglGetDisplay"); return false; }
    if (eglInitialize(s.display, nullptr, nullptr) != EGL_TRUE) { error = egl_error("eglInitialize"); return false; }
    eglBindAPI(EGL_OPENGL_ES_API);
#if defined(__ANDROID__)
    const EGLint surface_type = EGL_WINDOW_BIT | EGL_PBUFFER_BIT;
#else
    const EGLint surface_type = EGL_PBUFFER_BIT;
#endif
    const EGLint attributes[] = {EGL_RENDERABLE_TYPE, EGL_OPENGL_ES2_BIT, EGL_SURFACE_TYPE, surface_type,
                                 EGL_RED_SIZE, 8, EGL_GREEN_SIZE, 8, EGL_BLUE_SIZE, 8, EGL_ALPHA_SIZE, 8, EGL_NONE};
    EGLint count = 0;
    if (eglChooseConfig(s.display, attributes, &s.config, 1, &count) != EGL_TRUE || count < 1) {
        error = count < 1 ? "no RGBA8888 GLES2 EGL config" : egl_error("eglChooseConfig");
        return false;
    }
    const EGLint context_attributes[] = {EGL_CONTEXT_CLIENT_VERSION, 2, EGL_NONE};
    s.guest = eglCreateContext(s.display, s.config, EGL_NO_CONTEXT, context_attributes);
    if (s.guest == EGL_NO_CONTEXT) { error = egl_error("eglCreateContext"); return false; }
    s.shower = eglCreateContext(s.display, s.config, s.guest, context_attributes);
    if (s.shower == EGL_NO_CONTEXT) { error = egl_error("eglCreateContext(shared)"); return false; }
    const EGLint pbuffer_attributes[] = {EGL_WIDTH, 1, EGL_HEIGHT, 1, EGL_NONE};
    s.pbuffer = eglCreatePbufferSurface(s.display, s.config, pbuffer_attributes);
    if (s.pbuffer == EGL_NO_SURFACE && !has_extension(eglQueryString(s.display, EGL_EXTENSIONS), "EGL_KHR_surfaceless_context")) {
        error = egl_error("eglCreatePbufferSurface");
        return false;
    }
    if (!s.make_guest_current()) { error = egl_error("eglMakeCurrent"); return false; }
    const auto text = [&s](EGLint name) { const char* v = eglQueryString(s.display, name); return std::string(v ? v : "?"); };
    s.note("egl " + text(EGL_VENDOR) + " " + text(EGL_VERSION));
    return true;
}

void EglHost::present(std::uint32_t texture, std::uint32_t width, std::uint32_t height) {
    auto& s = *impl_;
    if (s.capture && width && height) {
        // The guest's framebuffer is bound: read it here, bottom row first, and hand it over top row first.
        const std::size_t stride = std::size_t{width} * 4;
        s.pixels.resize(stride * height);
        glReadPixels(0, 0, static_cast<GLsizei>(width), static_cast<GLsizei>(height), GL_RGBA, GL_UNSIGNED_BYTE, s.pixels.data());
        s.row.resize(stride);
        for (std::uint32_t y = 0; y < height / 2; ++y) {
            auto* top = s.pixels.data() + y * stride;
            auto* bottom = s.pixels.data() + (height - 1 - y) * stride;
            std::memcpy(s.row.data(), top, stride);
            std::memcpy(top, bottom, stride);
            std::memcpy(bottom, s.row.data(), stride);
        }
        s.capture(s.pixels, width, height);
    }
    GLint framebuffer = 0;
    glGetIntegerv(GL_FRAMEBUFFER_BINDING, &framebuffer);
    s.show(texture);
    glBindFramebuffer(GL_FRAMEBUFFER, static_cast<GLuint>(framebuffer));
}

void EglHost::set_window(ANativeWindow* window) {
    auto& s = *impl_;
    std::lock_guard lock(s.window_lock);
    if (window == s.window) return;
    // Never current outside show(), which holds the lock: safe to destroy here.
    s.drop_window_surface();
#if defined(__ANDROID__)
    if (s.window) ANativeWindow_release(s.window);
    if (window) ANativeWindow_acquire(window);
#endif
    s.window = window;
    s.window_failed = false;
}
}
