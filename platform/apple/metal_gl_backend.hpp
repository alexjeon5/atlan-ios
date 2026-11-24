// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include "atlan/gles.hpp"
#include <atomic>
#include <cstdint>
#include <memory>

namespace atlan {
// GlBackend that presents to a CAMetalLayer. `layer` is a CAMetalLayer* passed as void* so this header
// stays plain C++. Implemented so far: viewport, clear color/clear (render pass load action) and present on
// eglSwapBuffers. Draw calls are counted but not rendered yet (GLSL ES -> MSL translation is pending); the
// counters let the shell report what the guest asked for.
class MetalGlBackend final : public GlBackend {
public:
    explicit MetalGlBackend(void* layer);
    ~MetalGlBackend() override;
    GlResult execute(const GlCall& call) override;
    struct Stats {
        std::uint64_t frames{}, draws{}, draws_skipped{}, shaders{}, programs{}, textures{}, buffers{};
    };
    [[nodiscard]] Stats stats() const noexcept;
private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
}
