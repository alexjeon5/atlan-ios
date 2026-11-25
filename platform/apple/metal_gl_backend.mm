// SPDX-License-Identifier: GPL-2.0-or-later
#include "metal_gl_backend.hpp"
#include "atlan/gl_context.hpp"
#import <Metal/Metal.h>
#import <QuartzCore/CAMetalLayer.h>
#include <TargetConditionals.h>
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
MTLSamplerMinMagFilter filter(std::uint32_t gl) {  // mip chains are not uploaded, so mipmapped filters reduce to their base filter
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
        commands = [queue commandBuffer];
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
        if (const auto it = pipelines.find(key); it != pipelines.end()) return it->second;
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
        return state;
    }
    id<MTLDepthStencilState> depth_state(const GlRenderState& s) {
        const auto key = std::make_tuple(s.depth_test, s.depth_test ? s.depth_func : 0x207U, s.depth_test && s.depth_write);
        if (const auto it = depth_states.find(key); it != depth_states.end()) return it->second;
        MTLDepthStencilDescriptor* d = [MTLDepthStencilDescriptor new];
        d.depthCompareFunction = s.depth_test ? compare(s.depth_func) : MTLCompareFunctionAlways;
        d.depthWriteEnabled = s.depth_test && s.depth_write;
        id<MTLDepthStencilState> state = [device newDepthStencilStateWithDescriptor:d];
        depth_states[key] = state;
        return state;
    }
    id<MTLSamplerState> sampler_for(const GlSamplerParams& p) {
        const auto key = std::make_tuple(p.min_filter, p.mag_filter, p.wrap_s, p.wrap_t);
        if (const auto it = samplers.find(key); it != samplers.end()) return it->second;
        MTLSamplerDescriptor* d = [MTLSamplerDescriptor new];
        d.minFilter = filter(p.min_filter);
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
        MTLTextureDescriptor* d = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatRGBA8Unorm width:image->width height:image->height mipmapped:NO];
        d.usage = MTLTextureUsageShaderRead;
        id<MTLTexture> t = [device newTextureWithDescriptor:d];
        [t replaceRegion:MTLRegionMake2D(0, 0, image->width, image->height) mipmapLevel:0 withBytes:image->rgba.data() bytesPerRow:image->width * 4];
        textures[image->serial] = t;
        return t;
    }
    // Small payloads go inline; larger ones need a buffer.
    void set_vertex_data(const void* bytes, std::size_t length, NSUInteger index) {
        if (length <= 4096) [encoder setVertexBytes:bytes length:length atIndex:index];
        else [encoder setVertexBuffer:[device newBufferWithBytes:bytes length:length options:MTLResourceStorageModeShared] offset:0 atIndex:index];
    }

    void draw(const GlDraw& d) override {
        begin_frame();
        if (frame_skipped) { ++counters.draws_dropped; return; }
        id<MTLRenderPipelineState> pipeline = pipeline_for(d);
        if (!pipeline) { ++counters.draws_dropped; return; }
        const auto& s = d.state;
        if (s.cull && s.cull_face == 0x0408) return;  // GL_FRONT_AND_BACK: every triangle is culled
        ensure_encoder();
        [encoder setRenderPipelineState:pipeline];
        [encoder setDepthStencilState:depth_state(s)];
        [encoder setCullMode:!s.cull ? MTLCullModeNone : (s.cull_face == 0x0404 ? MTLCullModeFront : MTLCullModeBack)];
        [encoder setFrontFacingWinding:s.front_face == 0x0900 ? MTLWindingClockwise : MTLWindingCounterClockwise];
        const double w = static_cast<double>(target.width), h = static_cast<double>(target.height);
        MTLViewport vp{0, 0, w, h, 0, 1};
        if (s.viewport[2] > 0 && s.viewport[3] > 0)  // GL's origin is the lower left corner
            vp = MTLViewport{double(s.viewport[0]), h - double(s.viewport[1]) - double(s.viewport[3]), double(s.viewport[2]), double(s.viewport[3]), 0, 1};
        [encoder setViewport:vp];
        set_vertex_data(d.uniforms.data(), d.uniforms.size(), 0);
        [encoder setFragmentBytes:d.uniforms.data() length:d.uniforms.size() atIndex:0];
        for (std::size_t k = 0; k < d.streams.size(); ++k)
            set_vertex_data(d.streams[k].values.data(), d.streams[k].values.size() * sizeof(float), 1 + k);
        for (std::size_t t = 0; t < d.textures.size(); ++t) {
            [encoder setFragmentTexture:texture_for(d.textures[t].image) atIndex:t];
            [encoder setFragmentSamplerState:sampler_for(d.textures[t].sampler) atIndex:t];
        }
        id<MTLBuffer> indices = [device newBufferWithBytes:d.indices.data() length:d.indices.size() * sizeof(std::uint32_t) options:MTLResourceStorageModeShared];
        [encoder drawIndexedPrimitives:primitive(d.primitive) indexCount:d.indices.size() indexType:MTLIndexTypeUInt32 indexBuffer:indices indexBufferOffset:0];
        ++counters.draws;
    }
};

MetalGlBackend::MetalGlBackend(void* layer) : impl_(std::make_unique<Impl>()) {
    impl_->layer = (__bridge CAMetalLayer*)layer;
    if (!impl_->device) return;
    impl_->layer.device = impl_->device;
    impl_->layer.pixelFormat = MTLPixelFormatBGRA8Unorm;
    impl_->layer.framebufferOnly = YES;
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
