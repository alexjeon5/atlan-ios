// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include "atlan/gles.hpp"
#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace atlan {
// GlBackend that renders the guest's GLES2 calls with Metal. Calls are decoded and tracked by GlContext (GLSL ES is
// translated to MSL there and compiled here with newLibraryWithSource, which iOS permits: no JIT is involved);
// this class is its GlSink and encodes clears and draws into one render pass sequence per guest frame, presenting
// on eglSwapBuffers (or when the host swaps after renderGLSF).
class MetalGlBackend final : public GlBackend {
public:
    explicit MetalGlBackend(void* layer);                                 // CAMetalLayer*; sized by the embedder
    MetalGlBackend(std::uint32_t width, std::uint32_t height);            // offscreen BGRA8 target for tests and tools
    ~MetalGlBackend() override;
    GlResult execute(const GlCall& call) override;
    [[nodiscard]] bool available() const noexcept;                        // false when there is no Metal device
    // Offscreen only: pixels of the last presented frame, tightly packed BGRA8, top row first.
    bool read_back(std::vector<std::uint8_t>& bgra, std::uint32_t& width, std::uint32_t& height);
    void set_log(std::function<void(const std::string&)> log);
    struct Stats {
        std::uint64_t frames{}, draws{}, draws_dropped{}, clears{}, programs{}, shader_failures{}, link_failures{}, pipeline_failures{}, library_failures{};
    };
    [[nodiscard]] Stats stats() const noexcept;
private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
}
