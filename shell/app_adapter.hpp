// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include <cstddef>
#include <cstdint>
#include <functional>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

// The boundary between the engine (GuestSession and everything below it) and one app. The engine loads an image,
// runs its constructors and JNI_OnLoad, delivers input and draws frames; an AppAdapter supplies what differs per app:
// where its library is and how it is packaged, how its data folder is laid out, how its Java side answers, and which
// native entry points start it and draw a frame. Nothing app-specific belongs in the engine (docs/engine-adapter.md).
namespace atlan { class GuestStdio; class JavaBootstrap; }
namespace atlan::shell {
class SurfaceState;
using SessionLog = std::function<void(const std::string&)>;

// What the session offers an adapter once the image is loaded. Guest thread only.
class AppHost {
public:
    virtual ~AppHost() = default;
    [[nodiscard]] virtual JavaBootstrap& java() = 0;
    // Address of a function the image exports; 0 if there is none.
    [[nodiscard]] virtual std::uint32_t function(std::string_view symbol) const = 0;
    // A native method registered through RegisterNatives, else exported under its JNI name (Java_...); 0 if none.
    [[nodiscard]] virtual std::uint32_t native_method(std::string_view class_name, std::string_view name,
                                                      std::string_view signature) const = 0;
    // Calls guest code from the session's own loop (not from inside another guest call) with the per-call budget.
    // Returns r0. On any other outcome the session logs where the guest stopped, fails with `label`, and the result
    // is nullopt; the adapter then returns its own failure up to the session.
    virtual std::optional<std::uint32_t> call(const std::string& label, std::uint32_t address,
                                              std::span<const std::uint32_t> words) = 0;
    // Calls guest code from inside a Java method the guest invoked (a nested call). Returns r0, or nullopt with
    // `message` set to the dispatcher's reason (possibly empty).
    virtual std::optional<std::uint32_t> call_nested(std::uint32_t address, std::span<const std::uint32_t> words,
                                                     std::string& message) = 0;
    // Host surface size, if the embedder supplied one.
    [[nodiscard]] virtual const SurfaceState* surface() const = 0;
    // The session's log. A copy may be kept and called from any thread (the embedder's log is thread-safe).
    [[nodiscard]] virtual const SessionLog& log() const = 0;
};

// The native entry points that start the app and draw its frames. Each is called as a JNI instance method:
// (env, receiver[, extra]) where receiver is a new object of `receiver_class`.
struct AppStartup {
    std::string receiver_class;
    std::string init;  // exported function called once after JNI_OnLoad; empty or missing: no surfaces, no frames
    struct SurfaceCall {
        std::string symbol;
        bool surface_argument{};  // passes a new android/view/Surface object as the third word
    };
    std::vector<SurfaceCall> surface_calls;  // in order after init
    std::string render;     // draws one frame; the host swaps buffers after it
    std::string exit_poll;  // polled once a second while interactive; low byte 1 means the user quit. Empty: none
};

// One app. The session creates no adapter itself; without one it loads a bare ELF image, runs its constructors and
// JNI_OnLoad and stops there. Methods are called on the guest thread, in the order declared.
class AppAdapter {
public:
    virtual ~AppAdapter() = default;
    // The library to run when the embedder names none: a path under `data_root`, or empty.
    [[nodiscard]] virtual std::string find_library(const std::string& data_root) const { (void)data_root; return {}; }
    // Failure text when neither the embedder nor find_library() names a library.
    [[nodiscard]] virtual std::string missing_library() const { return "no guest library"; }
    // Replaces a packaged library (an APK, an app-specific wrapper) by the ELF image inside it. Bytes that are no
    // package are left alone. Throws std::runtime_error naming what is wrong with a damaged package.
    virtual void unpack_library(std::vector<std::byte>& bytes, const SessionLog& log) { (void)bytes; (void)log; }
    // Lays out the guest's view of the data folder (aliases, read filters) before stdio is installed.
    virtual void configure_files(GuestStdio& stdio, const SessionLog& log) { (void)stdio; (void)log; }
    // Registers the Java methods the guest calls, before JNI_OnLoad. `frame_requests`: the embedder wants frames
    // drawn only when the guest asks for one; frames_on_request() then says whether this app can.
    virtual void install_java(AppHost& host, bool frame_requests) { (void)host; (void)frame_requests; }
    // The session leaves its Java phase; the host and the bootstrap are gone after this. Drop what refers to them
    // (replies still in flight are discarded).
    virtual void uninstall_java() {}
    [[nodiscard]] virtual AppStartup startup() const { return {}; }
    // Delivers what the app's Java side would have delivered by now (replies, state changes) as guest calls, before
    // input and each frame. false: a call failed and the session has already failed.
    virtual bool deliver(AppHost& host) { (void)host; return true; }

    // Frames on request, as a GLSurfaceView in RENDERMODE_WHEN_DIRTY draws them.
    [[nodiscard]] virtual bool frames_on_request() const { return false; }
    // Whether the guest asked for a frame since the last frame_drawn().
    [[nodiscard]] virtual bool frame_requested() const { return false; }
    virtual void frame_drawn() {}
    // Requests since the last call (for the frame report).
    virtual std::uint64_t take_frame_request_count() { return 0; }
    // No request for a while: asks the guest whether a frame drawn now would show anything. nullopt: a call failed
    // and the session has already failed.
    virtual std::optional<bool> frame_ready(AppHost& host) { (void)host; return true; }
};
}
