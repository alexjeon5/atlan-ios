// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include "atlan/gl_context.hpp"
#include "atlan/gles.hpp"
#include <cstdio>
#include <map>
#include <string>

namespace atlan {
// Debug backend: forwards nothing, reports successful compile/link and prints one line per GL call to `out`.
// Draw calls include their attribute layouts; shader sources are printed only when `show_sources` is set.
class RecordingGlBackend final : public GlBackend {
public:
    explicit RecordingGlBackend(std::FILE* out, bool show_sources = false) : out_(out), show_sources_(show_sources) {}
    GlResult execute(const GlCall& call) override;
    std::map<std::string, std::uint64_t> counts;
private:
    std::FILE* out_;
    bool show_sources_;
};
const char* gl_op_name(GlOp op);

// GlSink that prints one line per clear/draw/present (program, primitive, counts, bound textures, first uniforms).
class LoggingGlSink final : public GlSink {
public:
    explicit LoggingGlSink(std::FILE* out) : out_(out) {}
    void clear(const GlClear& c) override;
    void draw(const GlDraw& d) override;
    void present() override;
    std::uint64_t draws{}, presents{};
private:
    std::FILE* out_;
};
}
