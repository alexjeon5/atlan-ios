// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include "gles_backend.hpp"
#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <vector>

struct ANativeWindow;
namespace atlan {
// GlesHost on EGL. The guest draws in its own context bound to a small pbuffer (or no surface); a second context in
// the same share group shows each finished frame on the window, stretched over all of it, when there is one. The
// window surface is current only while a frame is shown, under a lock, so the UI thread can take the window away
// at any time without waiting for the guest. Without a display (Linux, headless) the default display is the Mesa
// surfaceless platform when it exists.
class EglHost final : public GlesHost {
public:
    EglHost();
    ~EglHost() override;
    EglHost(const EglHost&) = delete;
    EglHost& operator=(const EglHost&) = delete;
    bool attach(std::string& error) override;  // guest thread
    void present(std::uint32_t texture, std::uint32_t width, std::uint32_t height) override;  // guest thread
    // The window frames are shown on (Android: from a SurfaceHolder). Any thread; nullptr when the window goes away.
    // Returns once the guest thread is not using the previous window.
    void set_window(ANativeWindow* window);
    // Called at each frame with its pixels (RGBA8, top row first), for headless tools and tests. Before attach().
    using Capture = std::function<void(const std::vector<std::uint8_t>& rgba, std::uint32_t width, std::uint32_t height)>;
    void set_capture(Capture capture);
    void set_log(std::function<void(const std::string&)> log);
    [[nodiscard]] std::uint64_t frames_shown() const noexcept;
private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
}
