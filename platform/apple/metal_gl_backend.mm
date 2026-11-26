// SPDX-License-Identifier: GPL-2.0-or-later
#include "metal_gl_backend.hpp"
#include "atlan/gl_context.hpp"
#import <Metal/Metal.h>
#import <QuartzCore/CAMetalLayer.h>
#include <TargetConditionals.h>
#include <algorithm>
#include <array>
#include <cstring>
#include <map>
#include <mutex>
#include <set>
#include <tuple>

namespace atlan {
namespace {
MTLBlendFactor blend_factor(std::uint32_t gl) {
    switch (gl) {
        case 0x0000: return MTLBlendFactorZero;
        case 0x0001: return MTLBlendFactorOne;
        case 0x0300: return MTLBlendFactorSourceColor;
        case 0x0301: return MTLBlendFactorOneMinusSourceColor;
        case 0x0302: return MTLBlendFactorSourceAlpha;
        case 0x0303: return MTLBlendFactorOneMinusSourceAlpha;
        case 0x0304: return MTLBlendFactorDestinationAlpha;
        case 0x0305: return MTLBlendFactorOneMinusDestinationAlpha;
        case 0x0306: return MTLBlendFactorDestinationColor;
        case 0x0307: return MTLBlendFactorOneMinusDestinationColor;
        case 0x0308: return MTLBlendFactorSourceAlphaSaturated;
        default: return MTLBlendFactorOne;
    }
}
MTLCompareFunction compare(std::uint32_t gl) {
    switch (gl) {
        case 0x0200: return MTLCompareFunctionNever;
        case 0x0201: return MTLCompareFunctionLess;
        case 0x0202: return MTLCompareFunctionEqual;
        case 0x0203: return MTLCompareFunctionLessEqual;
        case 0x0204: return MTLCompareFunctionGreater;
        case 0x0205: return MTLCompareFunctionNotEqual;
        case 0x0206: return MTLCompareFunctionGreaterEqual;
        default: return MTLCompareFunctionAlways;
    }
}
MTLSamplerMipFilter mip_filter(std::uint32_t gl) {
    switch (gl) {
        case 0x2700: case 0x2701: return MTLSamplerMipFilterNearest;
        case 0x2702: case 0x2703: return MTLSamplerMipFilterLinear;
        default: return MTLSamplerMipFilterNotMipmapped;
    }
}
MTLSamplerMinMagFilter filter(std::uint32_t gl) {  
    switch (gl) {
        case 0x2600: case 0x2700: case 0x2702: return MTLSamplerMinMagFilterNearest;
        default: return MTLSamplerMinMagFilterLinear;
    }
}
MTLSamplerAddressMode wrap(std::uint32_t gl) {
    switch (gl) {
        case 0x812F: return MTLSamplerAddressModeClampToEdge;
        case 0x8370: return MTLSamplerAddressModeMirrorRepeat;
        default: return MTLSamplerAddressModeRepeat;
    }
}
MTLVertexFormat vertex_format(std::uint32_t components) {
    switch (components) {
        case 1: return MTLVertexFormatFloat;
        case 2: return MTLVertexFormatFloat2;
        case 3: return MTLVertexFormatFloat3;
        default: return MTLVertexFormatFloat4;
    }
}
MTLPrimitiveType primitive(GlPrimitive p) {
    switch (p) {
        case GlPrimitive::points: return MTLPrimitiveTypePoint;
        case GlPrimitive::lines: return MTLPrimitiveTypeLine;
        case GlPrimitive::line_strip: return MTLPrimitiveTypeLineStrip;
        case GlPrimitive::triangles: return MTLPrimitiveTypeTriangle;
        case GlPrimitive::triangle_strip: return MTLPrimitiveTypeTriangleStrip;
    }
    return MTLPrimitiveTypeTriangle;
}
}

struct MetalGlBackend::Impl final : GlSink {
    // device
    CAMetalLayer* layer{};
    id<MTLDevice> device{};
    id<MTLCommandQueue> queue{};
    id<MTLTexture> offscreen{};
    id<MTLTexture> depth{};
    id<MTLTexture> black{};
    id<MTLTexture> readback{};  // CPU-readable copy of the layer's drawable, for glReadPixels
    MTLPixelFormat color_format{MTLPixelFormatBGRA8Unorm};
    // frame
    id<CAMetalDrawable> drawable{};
    id<MTLTexture> target{};
    id<MTLCommandBuffer> commands{};
    id<MTLRenderCommandEncoder> encoder{};
    bool frame_open{}, frame_skipped{}, color_valid{}, depth_valid{};
    bool pending_color{}, pending_depth{};
    GlClear pending{};
    // caches
    std::map<std::uint64_t, id<MTLLibrary>> libraries;
    std::map<std::tuple<std::uint64_t, bool, std::uint32_t, std::uint32_t>, id<MTLRenderPipelineState>> pipelines;
    std::map<std::tuple<bool, std::uint32_t, bool>, id<MTLDepthStencilState>> depth_states;
    std::map<std::tuple<std::uint32_t, std::uint32_t, std::uint32_t, std::uint32_t>, id<MTLSamplerState>> samplers;
    std::map<std::uint64_t, id<MTLTexture>> textures;
    std::set<std::uint64_t> broken_programs;
    std::function<void(const std::string&)> log;
    Stats counters;
    std::mutex lock;
    std::vector<std::uint8_t> last_frame;
    std::uint32_t last_width{}, last_height{};
    // Per-draw data (indices, large vertex streams) is copied into shared buffers that are reused every
    // `frames_in_flight` frames, instead of a new MTLBuffer per draw. A slot is reused once the command buffer
    // that last read it has completed.
    static constexpr std::size_t frames_in_flight = 3;
    static constexpr NSUInteger upload_chunk_bytes = NSUInteger{1} << 20;
    struct UploadSlot {
        std::vector<id<MTLBuffer>> chunks;
        std::size_t chunk{};
        NSUInteger offset{};
        id<MTLCommandBuffer> fence{};  // the last committed command buffer that used this slot
    };
    std::array<UploadSlot, frames_in_flight> uploads;
    std::size_t upload_slot{};
    struct Uploaded {
        id<MTLBuffer> buffer{};
        NSUInteger offset{};
    };
    std::vector<std::uint16_t> short_indices;
    // What the current encoder already has, so a draw only sends the state that changed.
    static constexpr std::size_t tracked_textures = 16;
    struct Bound {
        id<MTLRenderPipelineState> pipeline{};
        id<MTLDepthStencilState> depth{};
        int cull{-1}, winding{-1};
        bool has_viewport{};
        MTLViewport viewport{};
        std::array<bool, tracked_textures> has_texture{};
        std::array<std::uint64_t, tracked_textures> texture{};  // image serial, 0 for the black texture
        std::array<bool, tracked_textures> has_sampler{};
        std::array<GlSamplerParams, tracked_textures> sampler{};
    } bound;
    // The pipeline and depth state of the previous draw, which the next one usually repeats.
    std::tuple<std::uint64_t, bool, std::uint32_t, std::uint32_t> last_pipeline_key{};
    id<MTLRenderPipelineState> last_pipeline{};
    std::tuple<bool, std::uint32_t, bool> last_depth_key{};
    id<MTLDepthStencilState> last_depth{};
    GlContext context{*this};

    Impl() {
        device = MTLCreateSystemDefaultDevice();
        if (device) queue = [device newCommandQueue];
    }
    void note(const std::string& text) { if (log) log(text); }

    // ---- frame management
    void begin_frame() {
        if (frame_open) return;
        frame_open = true;
        frame_skipped = false;
        color_valid = depth_valid = false;
        if (!device) { frame_skipped = true; return; }
        if (layer) {
            drawable = [layer nextDrawable];
            if (!drawable) { frame_skipped = true; note("metal: no drawable is available, frame skipped"); return; }
            target = drawable.texture;
        } else {
            target = offscreen;
        }
        color_format = target.pixelFormat;
        if (!depth || depth.width != target.width || depth.height != target.height) {
            MTLTextureDescriptor* d = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatDepth32Float
                                                                                          width:target.width height:target.height mipmapped:NO];
            d.usage = MTLTextureUsageRenderTarget;
            d.storageMode = MTLStorageModePrivate;
            depth = [device newTextureWithDescriptor:d];
        }
        upload_slot = (upload_slot + 1) % frames_in_flight;
        auto& slot = uploads[upload_slot];
        if (slot.fence) {
            [slot.fence waitUntilCompleted];
            slot.fence = nil;
        }
        slot.chunk = 0;
        slot.offset = 0;
        commands = [queue commandBuffer];
    }
    // Copies `length` bytes into this frame's upload memory, at an offset that is a multiple of `alignment`.
    Uploaded upload(const void* bytes, std::size_t length, NSUInteger alignment) {
        auto& slot = uploads[upload_slot];
        NSUInteger at = (slot.offset + alignment - 1) & ~(alignment - 1);
        while (slot.chunk < slot.chunks.size() && at + length > slot.chunks[slot.chunk].length) {
            ++slot.chunk;
            at = 0;
        }
        if (slot.chunk == slot.chunks.size()) {
            id<MTLBuffer> fresh = [device newBufferWithLength:std::max<NSUInteger>(upload_chunk_bytes, length)
                                                      options:MTLResourceStorageModeShared | MTLResourceCPUCacheModeWriteCombined];
            if (!fresh) return {};
            slot.chunks.push_back(fresh);
            at = 0;
        }
        id<MTLBuffer> buffer = slot.chunks[slot.chunk];
        std::memcpy(static_cast<std::uint8_t*>(buffer.contents) + at, bytes, length);
        slot.offset = at + length;
        return {buffer, at};
    }
    void ensure_encoder() {
        if (encoder) return;
        MTLRenderPassDescriptor* pass = [MTLRenderPassDescriptor renderPassDescriptor];
        pass.colorAttachments[0].texture = target;
        pass.colorAttachments[0].storeAction = MTLStoreActionStore;
        if (pending_color || !color_valid) {
            pass.colorAttachments[0].loadAction = MTLLoadActionClear;
            const auto& c = pending_color ? pending.rgba : std::array<float, 4>{0, 0, 0, 1};
            pass.colorAttachments[0].clearColor = MTLClearColorMake(c[0], c[1], c[2], c[3]);
        } else {
            pass.colorAttachments[0].loadAction = MTLLoadActionLoad;
        }
        pass.depthAttachment.texture = depth;
        pass.depthAttachment.storeAction = MTLStoreActionStore;
        if (pending_depth || !depth_valid) {
            pass.depthAttachment.loadAction = MTLLoadActionClear;
            pass.depthAttachment.clearDepth = pending_depth ? pending.depth_value : 1.0;
        } else {
            pass.depthAttachment.loadAction = MTLLoadActionLoad;
        }
        encoder = [commands renderCommandEncoderWithDescriptor:pass];
        bound = Bound{};
        color_valid = depth_valid = true;
        pending_color = pending_depth = false;
    }
    void end_encoder() {
        if (!encoder) return;
        [encoder endEncoding];
        encoder = nil;
    }

    // ---- GlSink
    void clear(const GlClear& c) override {
        begin_frame();
        if (frame_skipped) return;
        end_encoder();  // a clear starts a new pass so its load action can do the work
        if (c.color) { pending_color = true; pending.rgba = c.rgba; }
        if (c.depth) { pending_depth = true; pending.depth_value = c.depth_value; }
    }
    // glReadPixels on the default framebuffer: RGBA8 rows from the bottom up. What has been drawn so far in this
    // frame is committed and copied out; the frame then continues in a new command buffer on the same target.
    bool read_pixels(std::int32_t x, std::int32_t y, std::uint32_t width, std::uint32_t height, std::vector<std::byte>& out) override {
        begin_frame();
        if (frame_skipped || !target || width == 0 || height == 0) return false;
        if (pending_color || pending_depth || !color_valid) ensure_encoder();
        end_encoder();
        const auto target_width = static_cast<std::uint32_t>(target.width), target_height = static_cast<std::uint32_t>(target.height);
        id<MTLTexture> source = target;
        if (source != offscreen) {  // a drawable's texture cannot be read directly: copy it to a shared one
            if (!readback || readback.width != target.width || readback.height != target.height || readback.pixelFormat != target.pixelFormat) {
                MTLTextureDescriptor* d = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:target.pixelFormat
                                                                                              width:target.width height:target.height mipmapped:NO];
                d.usage = MTLTextureUsageShaderRead;
#if TARGET_OS_OSX
                d.storageMode = device.hasUnifiedMemory ? MTLStorageModeShared : MTLStorageModeManaged;
#else
                d.storageMode = MTLStorageModeShared;
#endif
                readback = [device newTextureWithDescriptor:d];
            }
            if (!readback) return false;
            source = readback;
        }
        id<MTLBlitCommandEncoder> blit = [commands blitCommandEncoder];
        if (source != target)
            [blit copyFromTexture:target sourceSlice:0 sourceLevel:0 sourceOrigin:MTLOriginMake(0, 0, 0)
                       sourceSize:MTLSizeMake(target_width, target_height, 1)
                        toTexture:source destinationSlice:0 destinationLevel:0 destinationOrigin:MTLOriginMake(0, 0, 0)];
#if TARGET_OS_OSX
        if (!device.hasUnifiedMemory) [blit synchronizeResource:source];
#endif
        [blit endEncoding];
        [commands commit];
        [commands waitUntilCompleted];
        commands = [queue commandBuffer];
        std::vector<std::uint8_t> pixels(std::size_t{target_width} * target_height * 4);
        [source getBytes:pixels.data() bytesPerRow:target_width * 4 fromRegion:MTLRegionMake2D(0, 0, target_width, target_height) mipmapLevel:0];
        const bool bgra = source.pixelFormat == MTLPixelFormatBGRA8Unorm || source.pixelFormat == MTLPixelFormatBGRA8Unorm_sRGB;
        out.assign(std::size_t{width} * height * 4, std::byte{0});
        for (std::uint32_t row = 0; row < height; ++row) {
            const std::int64_t gl_row = std::int64_t{y} + row;  // from the bottom
            if (gl_row < 0 || gl_row >= target_height) continue;
            const auto* from = pixels.data() + (std::size_t{target_height} - 1 - static_cast<std::size_t>(gl_row)) * target_width * 4;
            auto* to = out.data() + std::size_t{row} * width * 4;
            for (std::uint32_t column = 0; column < width; ++column) {
                const std::int64_t gl_column = std::int64_t{x} + column;
                if (gl_column < 0 || gl_column >= target_width) continue;
                const auto* p = from + static_cast<std::size_t>(gl_column) * 4;
                to[column * 4 + 0] = std::byte{p[bgra ? 2 : 0]};
                to[column * 4 + 1] = std::byte{p[1]};
                to[column * 4 + 2] = std::byte{p[bgra ? 0 : 2]};
                to[column * 4 + 3] = std::byte{p[3]};
            }
        }
        return true;
    }
    void present() override {
        if (!frame_open) return;
        if (!frame_skipped) {
            if (pending_color || pending_depth || !color_valid) ensure_encoder();
            end_encoder();
            if (drawable) [commands presentDrawable:drawable];
#if TARGET_OS_OSX
            const bool read_back = offscreen != nil;
            if (read_back && !device.hasUnifiedMemory) {
                id<MTLBlitCommandEncoder> blit = [commands blitCommandEncoder];
                [blit synchronizeResource:offscreen];
                [blit endEncoding];
            }
#else
            const bool read_back = offscreen != nil;
#endif
            [commands commit];
            uploads[upload_slot].fence = commands;
            if (read_back) {
                [commands waitUntilCompleted];
                last_width = static_cast<std::uint32_t>(offscreen.width);
                last_height = static_cast<std::uint32_t>(offscreen.height);
                last_frame.resize(std::size_t{last_width} * last_height * 4);
                [offscreen getBytes:last_frame.data() bytesPerRow:last_width * 4 fromRegion:MTLRegionMake2D(0, 0, last_width, last_height) mipmapLevel:0];
            }
        }
        commands = nil;
        drawable = nil;
        target = nil;
        frame_open = false;
        pending_color = pending_depth = false;
        ++counters.frames;
    }
    void forget_texture(std::uint64_t serial) override { textures.erase(serial); }
    void forget_program(std::uint64_t serial) override {
        last_pipeline = nil;
        libraries.erase(serial);
        broken_programs.erase(serial);
        for (auto it = pipelines.begin(); it != pipelines.end();) it = std::get<0>(it->first) == serial ? pipelines.erase(it) : std::next(it);
    }
    std::string renderer() override { return device ? std::string("Metal ") + device.name.UTF8String : "Metal (unavailable)"; }

    // ---- cached objects
    id<MTLLibrary> library_for(const GlProgramInfo& program) {
        if (const auto it = libraries.find(program.serial); it != libraries.end()) return it->second;
        NSError* error = nil;
        NSString* source = [NSString stringWithUTF8String:program.linked.msl.c_str()];
        id<MTLLibrary> lib = [device newLibraryWithSource:source options:[MTLCompileOptions new] error:&error];
        if (!lib) {
            ++counters.library_failures;
            note(std::string("metal: shader library failed to compile: ") + (error ? error.localizedDescription.UTF8String : "unknown error"));
            broken_programs.insert(program.serial);
            return nil;
        }
        libraries[program.serial] = lib;
        return lib;
    }
    id<MTLRenderPipelineState> pipeline_for(const GlDraw& d) {
        const auto& s = d.state;
        const auto key = std::make_tuple(d.program->serial, s.blend, s.blend ? s.blend_src : 0U, s.blend ? s.blend_dst : 0U);
        if (last_pipeline && key == last_pipeline_key) return last_pipeline;
        if (const auto it = pipelines.find(key); it != pipelines.end()) {
            last_pipeline_key = key;
            last_pipeline = it->second;
            return it->second;
        }
        if (broken_programs.contains(d.program->serial)) return nil;
        id<MTLLibrary> lib = library_for(*d.program);
        if (!lib) return nil;
        MTLRenderPipelineDescriptor* desc = [MTLRenderPipelineDescriptor new];
        desc.vertexFunction = [lib newFunctionWithName:@"vs_main"];
        desc.fragmentFunction = [lib newFunctionWithName:@"fs_main"];
        MTLVertexDescriptor* vd = [MTLVertexDescriptor vertexDescriptor];
        for (std::size_t k = 0; k < d.streams.size(); ++k) {
            vd.attributes[k].format = vertex_format(d.streams[k].components);
            vd.attributes[k].offset = 0;
            vd.attributes[k].bufferIndex = 1 + k;
            vd.layouts[1 + k].stride = d.streams[k].components * sizeof(float);
            vd.layouts[1 + k].stepFunction = MTLVertexStepFunctionPerVertex;
            vd.layouts[1 + k].stepRate = 1;
        }
        if (!d.streams.empty()) desc.vertexDescriptor = vd;
        desc.colorAttachments[0].pixelFormat = color_format;
        desc.depthAttachmentPixelFormat = MTLPixelFormatDepth32Float;
        if (s.blend) {
            auto* a = desc.colorAttachments[0];
            a.blendingEnabled = YES;
            a.rgbBlendOperation = a.alphaBlendOperation = MTLBlendOperationAdd;
            a.sourceRGBBlendFactor = a.sourceAlphaBlendFactor = blend_factor(s.blend_src);
            a.destinationRGBBlendFactor = a.destinationAlphaBlendFactor = blend_factor(s.blend_dst);
        }
        NSError* error = nil;
        id<MTLRenderPipelineState> state = [device newRenderPipelineStateWithDescriptor:desc error:&error];
        if (!state) {
            ++counters.pipeline_failures;
            note(std::string("metal: pipeline creation failed: ") + (error ? error.localizedDescription.UTF8String : "unknown error"));
            broken_programs.insert(d.program->serial);
            return nil;
        }
        pipelines[key] = state;
        last_pipeline_key = key;
        last_pipeline = state;
        return state;
    }
    id<MTLDepthStencilState> depth_state(const GlRenderState& s) {
        const auto key = std::make_tuple(s.depth_test, s.depth_test ? s.depth_func : 0x207U, s.depth_test && s.depth_write);
        if (last_depth && key == last_depth_key) return last_depth;
        if (const auto it = depth_states.find(key); it != depth_states.end()) {
            last_depth_key = key;
            last_depth = it->second;
            return it->second;
        }
        MTLDepthStencilDescriptor* d = [MTLDepthStencilDescriptor new];
        d.depthCompareFunction = s.depth_test ? compare(s.depth_func) : MTLCompareFunctionAlways;
        d.depthWriteEnabled = s.depth_test && s.depth_write;
        id<MTLDepthStencilState> state = [device newDepthStencilStateWithDescriptor:d];
        depth_states[key] = state;
        last_depth_key = key;
        last_depth = state;
        return state;
    }
    id<MTLSamplerState> sampler_for(const GlSamplerParams& p) {
        const auto key = std::make_tuple(p.min_filter, p.mag_filter, p.wrap_s, p.wrap_t);
        if (const auto it = samplers.find(key); it != samplers.end()) return it->second;
        MTLSamplerDescriptor* d = [MTLSamplerDescriptor new];
        d.minFilter = filter(p.min_filter);
        d.mipFilter = mip_filter(p.min_filter);
        d.magFilter = filter(p.mag_filter);
        d.sAddressMode = wrap(p.wrap_s);
        d.tAddressMode = wrap(p.wrap_t);
        id<MTLSamplerState> state = [device newSamplerStateWithDescriptor:d];
        samplers[key] = state;
        return state;
    }
    id<MTLTexture> texture_for(const std::shared_ptr<const GlTextureImage>& image) {
        if (!image) {
            if (!black) {
                MTLTextureDescriptor* d = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatRGBA8Unorm width:1 height:1 mipmapped:NO];
                black = [device newTextureWithDescriptor:d];
                const std::uint8_t pixel[4] = {0, 0, 0, 255};
                [black replaceRegion:MTLRegionMake2D(0, 0, 1, 1) mipmapLevel:0 withBytes:pixel bytesPerRow:4];
            }
            return black;
        }
        if (const auto it = textures.find(image->serial); it != textures.end()) return it->second;
        const bool chain = !image->mips.empty();
        MTLTextureDescriptor* d = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatRGBA8Unorm width:image->width height:image->height mipmapped:chain];
        d.usage = MTLTextureUsageShaderRead;
        id<MTLTexture> t = [device newTextureWithDescriptor:d];
        [t replaceRegion:MTLRegionMake2D(0, 0, image->width, image->height) mipmapLevel:0 withBytes:image->rgba.data() bytesPerRow:image->width * 4];
        for (std::size_t l = 0; l < image->mips.size(); ++l) {
            const auto& m = image->mips[l];
            [t replaceRegion:MTLRegionMake2D(0, 0, m.width, m.height) mipmapLevel:l + 1 withBytes:m.rgba.data() bytesPerRow:m.width * 4];
        }
        textures[image->serial] = t;
        return t;
    }
    // Small payloads go inline; larger ones into this frame's upload memory.
    bool set_vertex_data(const void* bytes, std::size_t length, NSUInteger index, NSUInteger alignment) {
        if (length <= 4096) {
            [encoder setVertexBytes:bytes length:length atIndex:index];
            return true;
        }
        const Uploaded data = upload(bytes, length, alignment);
        if (!data.buffer) return false;
        [encoder setVertexBuffer:data.buffer offset:data.offset atIndex:index];
        return true;
    }

    void draw(const GlDraw& d) override {
        begin_frame();
        if (frame_skipped) { ++counters.draws_dropped; return; }
        id<MTLRenderPipelineState> pipeline = pipeline_for(d);
        if (!pipeline) { ++counters.draws_dropped; return; }
        const auto& s = d.state;
        if (s.cull && s.cull_face == 0x0408) return;  // GL_FRONT_AND_BACK: every triangle is culled
        // Indices: 16-bit while every vertex number fits below the strip restart value 0xFFFF.
        Uploaded indices;
        MTLIndexType index_type = MTLIndexTypeUInt32;
        if (d.vertex_count <= 0xFFFFU) {
            short_indices.resize(d.indices.size());
            for (std::size_t i = 0; i < d.indices.size(); ++i) short_indices[i] = static_cast<std::uint16_t>(d.indices[i]);
            indices = upload(short_indices.data(), short_indices.size() * sizeof(std::uint16_t), 16);
            index_type = MTLIndexTypeUInt16;
        } else {
            indices = upload(d.indices.data(), d.indices.size() * sizeof(std::uint32_t), 16);
        }
        if (!indices.buffer) { ++counters.draws_dropped; return; }
        ensure_encoder();
        if (pipeline != bound.pipeline) {
            [encoder setRenderPipelineState:pipeline];
            bound.pipeline = pipeline;
        }
        id<MTLDepthStencilState> depth_stencil = depth_state(s);
        if (depth_stencil != bound.depth) {
            [encoder setDepthStencilState:depth_stencil];
            bound.depth = depth_stencil;
        }
        const MTLCullMode cull = !s.cull ? MTLCullModeNone : (s.cull_face == 0x0404 ? MTLCullModeFront : MTLCullModeBack);
        if (static_cast<int>(cull) != bound.cull) {
            [encoder setCullMode:cull];
            bound.cull = static_cast<int>(cull);
        }
        const MTLWinding winding = s.front_face == 0x0900 ? MTLWindingClockwise : MTLWindingCounterClockwise;
        if (static_cast<int>(winding) != bound.winding) {
            [encoder setFrontFacingWinding:winding];
            bound.winding = static_cast<int>(winding);
        }
        const double w = static_cast<double>(target.width), h = static_cast<double>(target.height);
        MTLViewport vp{0, 0, w, h, 0, 1};
        if (s.viewport[2] > 0 && s.viewport[3] > 0)  // GL's origin is the lower left corner
            vp = MTLViewport{double(s.viewport[0]), h - double(s.viewport[1]) - double(s.viewport[3]), double(s.viewport[2]), double(s.viewport[3]), 0, 1};
        const auto& b = bound.viewport;
        if (!bound.has_viewport || vp.originX != b.originX || vp.originY != b.originY || vp.width != b.width || vp.height != b.height ||
            vp.znear != b.znear || vp.zfar != b.zfar) {
            [encoder setViewport:vp];
            bound.viewport = vp;
            bound.has_viewport = true;
        }
        if (!set_vertex_data(d.uniforms.data(), d.uniforms.size(), 0, 256)) { ++counters.draws_dropped; return; }
        [encoder setFragmentBytes:d.uniforms.data() length:d.uniforms.size() atIndex:0];
        for (std::size_t k = 0; k < d.streams.size(); ++k)
            if (!set_vertex_data(d.streams[k].values.data(), d.streams[k].values.size() * sizeof(float), 1 + k, 16)) { ++counters.draws_dropped; return; }
        for (std::size_t t = 0; t < d.textures.size(); ++t) {
            const auto& binding = d.textures[t];
            const std::uint64_t serial = binding.image ? binding.image->serial : 0;
            if (t >= tracked_textures || !bound.has_texture[t] || bound.texture[t] != serial) {
                [encoder setFragmentTexture:texture_for(binding.image) atIndex:t];
                if (t < tracked_textures) {
                    bound.has_texture[t] = true;
                    bound.texture[t] = serial;
                }
            }
            if (t >= tracked_textures || !bound.has_sampler[t] || !(bound.sampler[t] == binding.sampler)) {
                [encoder setFragmentSamplerState:sampler_for(binding.sampler) atIndex:t];
                if (t < tracked_textures) {
                    bound.has_sampler[t] = true;
                    bound.sampler[t] = binding.sampler;
                }
            }
        }
        [encoder drawIndexedPrimitives:primitive(d.primitive) indexCount:d.indices.size() indexType:index_type indexBuffer:indices.buffer indexBufferOffset:indices.offset];
        ++counters.draws;
    }
};

MetalGlBackend::MetalGlBackend(void* layer) : impl_(std::make_unique<Impl>()) {
    impl_->layer = (__bridge CAMetalLayer*)layer;
    if (!impl_->device) return;
    impl_->layer.device = impl_->device;
    impl_->layer.pixelFormat = MTLPixelFormatBGRA8Unorm;
    impl_->layer.framebufferOnly = NO;  // glReadPixels copies the drawable out
    impl_->layer.opaque = YES;  // GL's default framebuffer has no alpha to composite with
}
MetalGlBackend::MetalGlBackend(std::uint32_t width, std::uint32_t height) : impl_(std::make_unique<Impl>()) {
    if (!impl_->device) return;
    MTLTextureDescriptor* d = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatBGRA8Unorm width:width height:height mipmapped:NO];
    d.usage = MTLTextureUsageRenderTarget | MTLTextureUsageShaderRead;
#if TARGET_OS_OSX
    d.storageMode = impl_->device.hasUnifiedMemory ? MTLStorageModeShared : MTLStorageModeManaged;
#else
    d.storageMode = MTLStorageModeShared;
#endif
    impl_->offscreen = [impl_->device newTextureWithDescriptor:d];
}
MetalGlBackend::~MetalGlBackend() = default;
bool MetalGlBackend::available() const noexcept { return impl_->device != nil && (impl_->layer != nil || impl_->offscreen != nil); }
bool MetalGlBackend::read_back(std::vector<std::uint8_t>& bgra, std::uint32_t& width, std::uint32_t& height) {
    std::lock_guard guard(impl_->lock);
    if (impl_->last_frame.empty()) return false;
    bgra = impl_->last_frame;
    width = impl_->last_width;
    height = impl_->last_height;
    return true;
}
void MetalGlBackend::set_log(std::function<void(const std::string&)> log) {
    std::lock_guard guard(impl_->lock);
    impl_->log = log;
    impl_->context.set_log(std::move(log));
}
MetalGlBackend::Stats MetalGlBackend::stats() const noexcept {
    std::lock_guard guard(impl_->lock);
    Stats s = impl_->counters;
    const auto& c = impl_->context.stats();
    s.clears = c.clears;
    s.programs = c.programs_linked;
    s.shader_failures = c.shader_failures;
    s.link_failures = c.link_failures;
    s.draws_dropped += c.draws_dropped;
    return s;
}
GlResult MetalGlBackend::execute(const GlCall& call) {
    std::lock_guard guard(impl_->lock);
    @autoreleasepool {
        return impl_->context.execute(call);
    }
}
}
