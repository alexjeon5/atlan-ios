// SPDX-License-Identifier: GPL-2.0-or-later
#include "metal_gl_backend.hpp"
#import <Metal/Metal.h>
#import <QuartzCore/CAMetalLayer.h>
#include <mutex>

namespace atlan {
struct MetalGlBackend::Impl {
    CAMetalLayer* layer{};
    id<MTLDevice> device{};
    id<MTLCommandQueue> queue{};
    std::mutex lock;
    float clear[4]{0, 0, 0, 1};
    bool clear_requested{};
    std::uint32_t error{};
    Stats stats;
};

MetalGlBackend::MetalGlBackend(void* layer) : impl_(std::make_unique<Impl>()) {
    impl_->layer = (__bridge CAMetalLayer*)layer;
    impl_->device = MTLCreateSystemDefaultDevice();
    impl_->layer.device = impl_->device;
    impl_->layer.pixelFormat = MTLPixelFormatBGRA8Unorm;
    impl_->layer.framebufferOnly = YES;
    impl_->queue = [impl_->device newCommandQueue];
}
MetalGlBackend::~MetalGlBackend() = default;

MetalGlBackend::Stats MetalGlBackend::stats() const noexcept {
    std::lock_guard guard(impl_->lock);
    return impl_->stats;
}

GlResult MetalGlBackend::execute(const GlCall& call) {
    std::lock_guard guard(impl_->lock);
    auto& s = impl_->stats;
    GlResult r;
    switch (call.op) {
        case GlOp::clear_color:
            for (int i = 0; i < 4; ++i) impl_->clear[i] = call.f[i];
            break;
        case GlOp::clear:
            if (call.a[0] & 0x4000) impl_->clear_requested = true;  // GL_COLOR_BUFFER_BIT
            break;
        case GlOp::create_shader: ++s.shaders; break;
        case GlOp::create_program: ++s.programs; break;
        case GlOp::gen_texture: ++s.textures; break;
        case GlOp::gen_buffer: ++s.buffers; break;
        case GlOp::draw_arrays:
        case GlOp::draw_elements: ++s.draws; ++s.draws_skipped; break;
        case GlOp::get_program_iv:
        case GlOp::get_shader_iv: r.value = 1; break;  // compile/link status: true (translation pending)
        case GlOp::get_string: r.text = "atlan Metal"; break;
        case GlOp::get_error: r.value = impl_->error; break;
        case GlOp::swap_buffers: {
            @autoreleasepool {
                id<CAMetalDrawable> drawable = [impl_->layer nextDrawable];
                if (!drawable) break;
                MTLRenderPassDescriptor* pass = [MTLRenderPassDescriptor renderPassDescriptor];
                pass.colorAttachments[0].texture = drawable.texture;
                pass.colorAttachments[0].loadAction = MTLLoadActionClear;
                pass.colorAttachments[0].storeAction = MTLStoreActionStore;
                pass.colorAttachments[0].clearColor =
                    MTLClearColorMake(impl_->clear[0], impl_->clear[1], impl_->clear[2], impl_->clear[3]);
                id<MTLCommandBuffer> buffer = [impl_->queue commandBuffer];
                id<MTLRenderCommandEncoder> encoder = [buffer renderCommandEncoderWithDescriptor:pass];
                [encoder endEncoding];
                [buffer presentDrawable:drawable];
                [buffer commit];
                ++s.frames;
                impl_->clear_requested = false;
            }
            break;
        }
        default: break;
    }
    return r;
}
}
