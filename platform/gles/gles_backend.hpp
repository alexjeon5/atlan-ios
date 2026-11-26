// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include "atlan/gles.hpp"
#include <atomic>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>

namespace atlan {
// Supplies the GLES2 context a GlesBackend draws with and shows the frames it finishes.
class GlesHost {
public:
    virtual ~GlesHost() = default;
    // Makes a GLES2 context current on the calling thread (the guest thread). Called once, before the first GL call.
    // false with `error` set when there is none; the backend then drops every call.
    virtual bool attach(std::string& error) = 0;
    // A finished guest frame: `texture` (GL_TEXTURE_2D, RGBA8, origin bottom-left, `width` x `height`) in the
    // backend's context, which is current and has the guest's framebuffer bound. The host shows it (or keeps it) and
    // returns with the same context current and the same binding.
    virtual void present(std::uint32_t texture, std::uint32_t width, std::uint32_t height) = 0;
};

// GlBackend that hands the guest's GLES2 calls to a real GLES2 implementation (Android, or Mesa on Linux for
// tests). Shaders are the guest's own GLSL ES sources and compressed textures go to the driver as they are, so
// nothing is translated. Object ids the import service allocated map to the driver's names here.
//
// The guest's default framebuffer is an offscreen framebuffer of the guest's surface size (colour texture plus
// depth, 4x multisampled where EXT_multisampled_render_to_texture exists, like the original's EGL config); each
// eglSwapBuffers presents its texture through the host. The host's window can come and go without the guest's
// GL state or frame noticing.
class GlesBackend final : public GlBackend {
public:
    explicit GlesBackend(GlesHost& host);
    ~GlesBackend() override;
    GlesBackend(const GlesBackend&) = delete;
    GlesBackend& operator=(const GlesBackend&) = delete;
    // Guest thread only, like every GlBackend call.
    GlResult execute(const GlCall& call) override;
    // Size of the guest's default framebuffer. Any thread; applied before the next call on the guest thread (the
    // framebuffer's contents are lost then).
    void set_surface_size(std::uint32_t width, std::uint32_t height) noexcept;
    // Off: plain single-sampled framebuffer. Before the first call.
    void set_multisample(bool on) noexcept { multisample_ = on; }
    void set_log(std::function<void(const std::string&)> log);
    struct Stats {
        std::uint64_t frames{}, draws{}, draws_skipped{}, shader_failures{}, link_failures{};
    };
    [[nodiscard]] Stats stats() const noexcept;
private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
    std::atomic<std::uint64_t> pending_size_{0};  // (width << 32) | height; 0 = unchanged
    bool multisample_{true};
};
}
