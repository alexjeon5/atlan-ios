// SPDX-License-Identifier: GPL-2.0-or-later
#include "apple_audio_sink.hpp"
#import <AudioToolbox/AudioToolbox.h>
#include <TargetConditionals.h>
#if TARGET_OS_IOS
#import <AVFoundation/AVFoundation.h>
#endif
#include <algorithm>
#include <atomic>
#include <cstring>
#include <deque>
#include <mutex>

namespace atlan {
namespace {
constexpr UInt32 queue_buffers = 3;
constexpr std::size_t ring_limit = 8U << 20;

class QueueStream final : public AudioStream {
public:
    QueueStream(const PcmFormat& format) : format_(format) {}
    ~QueueStream() override {
        if (queue_ != nullptr) AudioQueueDispose(queue_, true);  // waits for the callback to finish
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
        OSStatus status = AudioQueueNewOutput(&description, &QueueStream::render, this, nullptr, nullptr, 0, &queue_);
        if (status != noErr) { error = "AudioQueueNewOutput failed: " + std::to_string(status); queue_ = nullptr; return false; }
        buffer_bytes_ = static_cast<UInt32>(std::max<std::size_t>(format_.sample_rate / 50, 256) * format_.frame_bytes());
        for (UInt32 i = 0; i < queue_buffers; ++i) {
            status = AudioQueueAllocateBuffer(queue_, buffer_bytes_, &buffers_[i]);
            if (status != noErr) { error = "AudioQueueAllocateBuffer failed: " + std::to_string(status); return false; }
        }
        return true;
    }
    bool append(std::span<const std::byte> pcm) override {
        std::lock_guard lock(mutex_);
        if (ring_.size() + pcm.size() > ring_limit) return false;
        ring_.insert(ring_.end(), pcm.begin(), pcm.end());
        return true;
    }
    void flush() override {
        std::lock_guard lock(mutex_);
        ring_.clear();
    }
    void set_playing(bool playing) override {
        if (playing == playing_) return;
        playing_ = playing;
        if (playing) {
#if TARGET_OS_IOS
            AVAudioSession* session = [AVAudioSession sharedInstance];
            [session setCategory:AVAudioSessionCategoryPlayback
                            mode:AVAudioSessionModeVoicePrompt
                         options:AVAudioSessionCategoryOptionDuckOthers | AVAudioSessionCategoryOptionInterruptSpokenAudioAndMixWithOthers
                           error:nil];
            [session setActive:YES error:nil];
#endif
            if (!primed_) {
                for (UInt32 i = 0; i < queue_buffers; ++i) fill_and_enqueue(buffers_[i]);
                primed_ = true;
            }
            AudioQueueStart(queue_, nullptr);
        } else {
            AudioQueuePause(queue_);
        }
    }
    void set_gain(float gain) override {
        AudioQueueSetParameter(queue_, kAudioQueueParam_Volume, std::clamp(gain, 0.0F, 1.0F));
    }
    std::uint64_t played_bytes() const override { return played_.load(std::memory_order_acquire); }
private:
    static void render(void* user, AudioQueueRef, AudioQueueBufferRef buffer) {
        static_cast<QueueStream*>(user)->fill_and_enqueue(buffer);
    }
    void fill_and_enqueue(AudioQueueBufferRef buffer) {
        std::size_t copied = 0;
        {
            std::lock_guard lock(mutex_);
            copied = std::min<std::size_t>(ring_.size(), buffer_bytes_);
            std::copy_n(ring_.begin(), copied, static_cast<std::byte*>(buffer->mAudioData));
            ring_.erase(ring_.begin(), ring_.begin() + static_cast<std::ptrdiff_t>(copied));
            played_.fetch_add(copied, std::memory_order_release);
        }
        std::memset(static_cast<char*>(buffer->mAudioData) + copied, 0, buffer_bytes_ - copied);
        buffer->mAudioDataByteSize = buffer_bytes_;
        AudioQueueEnqueueBuffer(queue_, buffer, 0, nullptr);
    }
    PcmFormat format_;
    AudioQueueRef queue_{};
    AudioQueueBufferRef buffers_[queue_buffers]{};
    UInt32 buffer_bytes_{};
    std::mutex mutex_;
    std::deque<std::byte> ring_;
    std::atomic<std::uint64_t> played_{0};
    bool playing_{false}, primed_{false};
};
}

std::unique_ptr<AudioStream> AppleAudioSink::open(const PcmFormat& format, std::string& error) {
    auto stream = std::make_unique<QueueStream>(format);
    if (!stream->start(error)) return nullptr;
    return stream;
}
}
