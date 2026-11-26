// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include "atlan/gles.hpp"
#include "atlan/glsl.hpp"
#include <array>
#include <functional>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

namespace atlan {
// The GLES2 state machine behind the guest's calls. GlContext tracks objects (shaders, programs, buffers, textures)
// and fixed-function state, translates linked programs to MSL (glsl.hpp) and turns every draw call into a
// self-contained GlDraw: converted vertex streams, resolved indices, a uniform block snapshot, texture snapshots
// and the render state. A GlSink (Metal on Apple hosts, a recorder in tests) only has to execute these.
enum class GlPrimitive : std::uint8_t { points, lines, line_strip, triangles, triangle_strip };

struct GlTextureLevel {
    std::uint32_t width{}, height{};
    std::vector<std::uint8_t> rgba;
};
struct GlTextureImage {          // immutable RGBA8 snapshot: width/height/rgba are level 0
    std::uint32_t width{}, height{};
    std::uint64_t serial{};      // unique per snapshot; sinks cache device textures by it
    std::vector<std::uint8_t> rgba;
    std::vector<GlTextureLevel> mips;  // levels 1.. of a complete chain down to 1x1; empty when the chain is absent or incomplete
};

struct GlSamplerParams {
    std::uint32_t min_filter{0x2702}, mag_filter{0x2601};  // GL_NEAREST_MIPMAP_LINEAR, GL_LINEAR
    std::uint32_t wrap_s{0x2901}, wrap_t{0x2901};          // GL_REPEAT
    bool operator==(const GlSamplerParams&) const = default;
};

struct GlProgramInfo {           // a successfully linked program; `serial` identifies it for sink-side caches
    std::uint64_t serial{};
    glsl::Linked linked;
};

struct GlRenderState {
    std::int32_t viewport[4]{0, 0, 0, 0};  // GL window coordinates (origin lower left); zero size = whole surface
    bool blend{}, cull{}, depth_test{}, depth_write{true};
    std::uint32_t blend_src{1}, blend_dst{0};        // GL_ONE, GL_ZERO
    std::uint32_t cull_face{0x0405};                 // GL_BACK
    std::uint32_t front_face{0x0901};                // GL_CCW
    std::uint32_t depth_func{0x0201};                // GL_LESS
};

struct GlAttributeStream {
    std::uint32_t components{};      // 1-4: the shader attribute's width
    std::vector<float> values;       // vertex_count * components, missing source components filled with (0, 0, 0, 1)
};

struct GlDraw {
    std::shared_ptr<const GlProgramInfo> program;
    std::vector<std::byte> uniforms;                 // program->linked.uniform_bytes
    std::vector<GlAttributeStream> streams;          // one per program attribute, in location order
    std::uint32_t vertex_count{};
    GlPrimitive primitive{GlPrimitive::triangles};
    std::vector<std::uint32_t> indices;              // always present (line loops and fans are already expanded)
    struct Binding {
        std::shared_ptr<const GlTextureImage> image; // null when nothing complete is bound: sample as black
        GlSamplerParams sampler;
    };
    std::vector<Binding> textures;                   // one per program sampler
    GlRenderState state;
};

struct GlClear {
    bool color{}, depth{};
    std::array<float, 4> rgba{};
    float depth_value{1};
};

class GlSink {
public:
    virtual ~GlSink() = default;
    virtual void clear(const GlClear& clear) = 0;
    virtual void draw(const GlDraw& draw) = 0;
    virtual void present() = 0;     // eglSwapBuffers, or the host's end of a guest frame
    virtual void forget_texture(std::uint64_t) {}
    virtual void forget_program(std::uint64_t) {}
    virtual bool read_pixels(std::int32_t, std::int32_t, std::uint32_t, std::uint32_t, std::vector<std::byte>&) { return false; }
    virtual std::string renderer() { return "atlan"; }
};

class GlContext final : public GlBackend {
public:
    explicit GlContext(GlSink& sink) : sink_(sink) {}
    GlResult execute(const GlCall& call) override;

    struct Stats {
        std::uint64_t frames{}, draws{}, draws_dropped{}, clears{}, programs_linked{}, link_failures{}, shader_failures{},
                      texture_uploads{}, unsupported{};
    };
    [[nodiscard]] const Stats& stats() const noexcept { return stats_; }
    // Diagnostics (shader/link failures, dropped draws, unsupported calls); called on the guest thread.
    void set_log(std::function<void(const std::string&)> log) { log_ = std::move(log); }

private:
    struct Shader {
        std::uint32_t type{};
        std::string source;
        std::shared_ptr<glsl::ShaderUnit> unit;
        bool compiled{};
        std::string log;
    };
    struct Program {
        std::vector<std::uint32_t> shaders;
        std::shared_ptr<GlProgramInfo> info;
        std::vector<std::byte> uniforms;
        std::vector<std::int32_t> sampler_units;
        std::string log;
    };
    struct Buffer { std::vector<std::byte> data; };
    struct Texture {
        std::vector<GlTextureLevel> levels;              // as uploaded, indexed by level
        std::shared_ptr<const GlTextureImage> image;     // snapshot of `levels`, rebuilt lazily
        bool dirty{};
        GlSamplerParams sampler;
        bool bound_once{};
    };
    struct Attribute {
        bool enabled{};
        std::uint32_t size{4}, type{0x1406}, stride{}, pointer{}, buffer{};
        bool normalized{};
    };

    void note(const std::string& text) const { if (log_) log_(text); }
    void fail(std::uint32_t code) { if (error_ == 0) error_ = code; }
    void compile_shader(std::uint32_t id);
    void link_program(std::uint32_t id);
    void set_uniform(const GlCall& call);
    void tex_image(const GlCall& call);
    void draw(const GlCall& call);
    GlResult query_shader(const GlCall& call) const;
    GlResult query_program(const GlCall& call) const;
    [[nodiscard]] Texture* bound_texture(std::uint32_t unit);
    void refresh(Texture& texture);

    GlSink& sink_;
    std::function<void(const std::string&)> log_;
    Stats stats_;
    std::uint32_t error_{};
    std::unordered_map<std::uint32_t, Shader> shaders_;
    std::unordered_map<std::uint32_t, Program> programs_;
    std::unordered_map<std::uint32_t, Buffer> buffers_;
    std::unordered_map<std::uint32_t, Texture> textures_;
    std::uint32_t current_program_{}, array_buffer_{}, element_buffer_{};
    std::array<std::uint32_t, 8> unit_texture_{};
    std::uint32_t active_unit_{};
    std::array<Attribute, 16> attributes_{};
    GlRenderState state_;
    GlClear clear_;
    std::uint32_t unpack_alignment_{4};
    std::uint64_t next_serial_{1};
    GlDraw scratch_;                  // the packet draw() fills and hands to the sink
    std::vector<std::uint32_t> fan_;  // a fan's indices before draw() expands them
};
}
