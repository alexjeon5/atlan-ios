// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include <atomic>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <vector>
#include "shell/http_client.hpp"
#include "shell/input.hpp"
#include "shell/surface.hpp"

namespace atlan { class AudioSink; class GlBackend; }
namespace atlan::shell {
enum class SessionState : std::uint8_t { idle, running, finished, failed };

struct SessionConfig {
    // User-supplied ARM32 ELF32 shared object, or an APK / encoded atlan.dat holding one (shell/app_package.hpp).
    // Empty uses the package find_package() finds in data_root.
    std::string library_path;
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
    // Host input. Events pushed to `input` (from any thread) are delivered on the guest thread before each rendered
    // frame, through the native methods named in `input_bindings`. An event without a binding, or whose method the
    // guest did not register or export, is reported once in the log and dropped.
    InputQueue* input{};
    InputBindings input_bindings;
    // Keep rendering frames (and delivering input) after the first `render_frames` until stop() or a guest stop.
    bool interactive{};
    std::uint32_t frame_interval_ms{33};          // lower bound on the time per interactive frame
    // Interactive frames are drawn only when the guest asks for one (its GL thread's callBackGLThread, answered as
    // the app's Java side does: isStateReady -> setState(3) -> request), as a GLSurfaceView in RENDERMODE_WHEN_DIRTY
    // would. Without a request the guest is asked once per `idle_redraw_ms` whether it is ready and a frame is
    // drawn only if so (a frame drawn while it is not ready shows just the clear colour). false: draw every interval.
    bool render_on_demand{};
    std::uint32_t idle_redraw_ms{1000};
    bool block_cache{true};  // false: single-step interpreter (comparison and diagnosis)
    // Replace run-time library functions inside the guest image by host implementations where the guest's own code
    // is found and agrees with them (atlan/native_functions.hpp). `native_signatures` optionally names a file of
    // byte signatures for functions the image does not export; a missing file is not an error.
    bool native_functions{true};
    std::string native_signatures;
    std::vector<std::string> native_skip;  // names of replacement sets to leave interpreted (diagnosis)
    // Non-empty: profile guest functions (atlan/profiler.hpp) and write `<profile_path>.<phase>.tsv` per phase:
    // init, each surface call, render (the first `render_frames` frames), frames (each interactive report
    // interval; repeated phases are numbered). Slows the guest down.
    std::string profile_path;
    // Network for the guest's Java side (shell/java_services.hpp): HTTP requests (nullptr: plain http:// only) and
    // whether a network is available (nullptr: always). Called from host threads and the guest thread respectively.
    HttpFetch http;
    std::function<bool()> network_available;
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
