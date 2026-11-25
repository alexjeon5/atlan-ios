// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include <atomic>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include "shell/surface.hpp"

namespace atlan { class AudioSink; class GlBackend; }
namespace atlan::shell {
enum class SessionState : std::uint8_t { idle, running, finished, failed };

struct SessionConfig {
    std::string library_path;                   // user-supplied ARM32 ELF32 shared object
    std::uint64_t initializer_budget{3000000};  // retired instructions per initializer
    AudioSink* audio_sink{};                    // device sink; nullptr uses a silent recording sink
    std::string data_root;                      // guest-visible file root (the app Documents folder); empty disables stdio
    std::uint64_t native_init_budget{200000000};  // retired instructions for MainActivity.nativeInit
    std::uint64_t surface_call_budget{2000000000};  // retired instructions per createUISF/createGLSF/changedUISF/renderGLSF call
    std::uint32_t render_frames{3};               // renderGLSF calls after the surface is created
    GlBackend* gl_backend{};                      // GLES2/EGL sink (Metal on devices); nullptr uses a no-op backend
    const SurfaceState* surface{};                // host surface size in native pixels; nullptr means unknown
    // Called on the guest thread when the guest reports MainActivity.setSurfaces(a, b). Arguments are passed through
    // unchanged; the embedder decides what they mean.
    std::function<void(std::int32_t, std::int32_t)> on_set_surfaces;
    bool block_cache{true};  // false: single-step interpreter (comparison and diagnosis)
    bool perf_log{};         // log instructions, seconds and MIPS per surface/render call, and block cache coverage
};

// Loads the guest image and runs its constructors on one dedicated thread.
// `log` is called from that thread; the embedder must hop to its UI thread.
// stop() is cooperative and takes effect between initializers.
class GuestSession {
public:
    using Log = std::function<void(const std::string&)>;
    GuestSession(SessionConfig config, Log log);
    ~GuestSession();
    GuestSession(const GuestSession&) = delete;
    GuestSession& operator=(const GuestSession&) = delete;
    bool start();  // false if already started or the thread cannot be created
    void stop() noexcept { stop_.store(true); }
    void join() noexcept;
    [[nodiscard]] SessionState state() const noexcept { return state_.load(); }
private:
    struct Thread;
    void run();
    SessionConfig config_;
    Log log_;
    std::atomic<bool> stop_{false};
    std::atomic<SessionState> state_{SessionState::idle};
    std::unique_ptr<Thread> thread_;
};
}
