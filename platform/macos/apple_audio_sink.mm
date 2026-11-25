// SPDX-License-Identifier: GPL-2.0-or-later
#include "apple_audio_sink.hpp"
#import <AudioToolbox/AudioToolbox.h>
#include <TargetConditionals.h>
#if TARGET_OS_IOS
#import <AVFoundation/AVFoundation.h>
#endif
#include <dispatch/dispatch.h>
#include <algorithm>
#include <atomic>
#include <cstring>
#include <deque>
#include <memory>
#include <mutex>

namespace atlan {
namespace {
constexpr UInt32 queue_buffers = 3;
constexpr std::size_t ring_limit = 8U << 20;

// AudioQueueStart, AudioQueuePause, AudioQueueDispose and the audio session calls each take milliseconds and
// some wait for the device. They run on one serial queue, in the order the guest asked for them, so the guest
// thread (which also renders the frames) never waits for the audio hardware.
dispatch_queue_t control_queue() {
    static dispatch_queue_t queue = dispatch_queue_create("atlan.audio.control", DISPATCH_QUEUE_SERIAL);
    return queue;
}

// What the device callback touches. Shared with the control queue so a stream can be destroyed while its
// queue is still being disposed.
struct Core {
    AudioQueueRef queue{};
    AudioQueueBufferRef buffers[queue_buffers]{};
    UInt32 buffer_bytes{};
    std::mutex mutex;
    std::deque<std::byte> ring;
    std::atomic<std::uint64_t> played{0};
    bool primed{false};  // control queue only

    static void render(void* user, AudioQueueRef, AudioQueueBufferRef buffer) {
        static_cast<Core*>(user)->fill_and_enqueue(buffer);
    }
    void fill_and_enqueue(AudioQueueBufferRef buffer) {
        std::size_t copied = 0;
        {
            std::lock_guard lock(mutex);
            copied = std::min<std::size_t>(ring.size(), buffer_bytes);
            std::copy_n(ring.begin(), copied, static_cast<std::byte*>(buffer->mAudioData));
            ring.erase(ring.begin(), ring.begin() + static_cast<std::ptrdiff_t>(copied));
            played.fetch_add(copied, std::memory_order_release);
        }
        std::memset(static_cast<char*>(buffer->mAudioData) + copied, 0, buffer_bytes - copied);
        buffer->mAudioDataByteSize = buffer_bytes;
        AudioQueueEnqueueBuffer(queue, buffer, 0, nullptr);
    }
};

class QueueStream final : public AudioStream {
public:
    QueueStream(const PcmFormat& format) : format_(format), core_(std::make_shared<Core>()) {}
    ~QueueStream() override {
        if (core_->queue == nullptr) return;
        std::shared_ptr<Core> core = core_;
        // Disposing waits for a running callback; the block keeps the core alive until then.
        dispatch_async(control_queue(), ^{ AudioQueueDispose(core->queue, true); });
    }
    bool start(std::string& error) {
        AudioStreamBasicDescription description{};
        description.mSampleRate = format_.sample_rate;
        description.mFormatID = kAudioFormatLinearPCM;
        description.mFormatFlags = kLinearPCMFormatFlagIsSignedInteger | kLinearPCMFormatFlagIsPacked;
        description.mBytesPerPacket = static_cast<UInt32>(format_.frame_bytes());
        description.mFramesPerPacket = 1;
        description.mBytesPerFrame = static_cast<UInt32>(format_.frame_bytes());
        description.mChannelsPerFrame = format_.channels;
        description.mBitsPerChannel = 16;
        // Run loop arguments null: the queue renders on its own internal thread.
        OSStatus status = AudioQueueNewOutput(&description, &Core::render, core_.get(), nullptr, nullptr, 0, &core_->queue);
        if (status != noErr) { error = "AudioQueueNewOutput failed: " + std::to_string(status); core_->queue = nullptr; return false; }
        core_->buffer_bytes = static_cast<UInt32>(std::max<std::size_t>(format_.sample_rate / 50, 256) * format_.frame_bytes());
        for (UInt32 i = 0; i < queue_buffers; ++i) {
            status = AudioQueueAllocateBuffer(core_->queue, core_->buffer_bytes, &core_->buffers[i]);
            if (status != noErr) { error = "AudioQueueAllocateBuffer failed: " + std::to_string(status); return false; }
        }
        return true;
    }
    bool append(std::span<const std::byte> pcm) override {
        std::lock_guard lock(core_->mutex);
        if (core_->ring.size() + pcm.size() > ring_limit) return false;
        core_->ring.insert(core_->ring.end(), pcm.begin(), pcm.end());
        return true;
    }
    void flush() override {
        std::lock_guard lock(core_->mutex);
        core_->ring.clear();
    }
    void set_playing(bool playing) override {
        if (playing == playing_) return;
        playing_ = playing;
        std::shared_ptr<Core> core = core_;
        if (playing) {
            dispatch_async(control_queue(), ^{
#if TARGET_OS_IOS
                // Once per process: the session keeps its category and stays active between prompts.
                static bool active = false;
                if (!active) {
                    AVAudioSession* session = [AVAudioSession sharedInstance];
                    [session setCategory:AVAudioSessionCategoryPlayback
                                    mode:AVAudioSessionModeVoicePrompt
                                 options:AVAudioSessionCategoryOptionDuckOthers | AVAudioSessionCategoryOptionInterruptSpokenAudioAndMixWithOthers
                                   error:nil];
                    active = [session setActive:YES error:nil];
                }
#endif
                if (!core->primed) {
                    for (UInt32 i = 0; i < queue_buffers; ++i) core->fill_and_enqueue(core->buffers[i]);
                    core->primed = true;
                }
                AudioQueueStart(core->queue, nullptr);
            });
        } else {
            dispatch_async(control_queue(), ^{ AudioQueuePause(core->queue); });
        }
    }
    void set_gain(float gain) override {
        AudioQueueSetParameter(core_->queue, kAudioQueueParam_Volume, std::clamp(gain, 0.0F, 1.0F));
    }
    std::uint64_t played_bytes() const override { return core_->played.load(std::memory_order_acquire); }
private:
    PcmFormat format_;
    std::shared_ptr<Core> core_;
    bool playing_{false};
};
}

std::unique_ptr<AudioStream> AppleAudioSink::open(const PcmFormat& format, std::string& error) {
    auto stream = std::make_unique<QueueStream>(format);
    if (!stream->start(error)) return nullptr;
    return stream;
}
}
