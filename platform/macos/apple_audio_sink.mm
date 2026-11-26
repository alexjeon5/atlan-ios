// SPDX-License-Identifier: GPL-2.0-or-later
#include "apple_audio_sink.hpp"
#import <AudioToolbox/AudioToolbox.h>
#include <TargetConditionals.h>
#if TARGET_OS_IOS
#import <AVFoundation/AVFoundation.h>
#import <UIKit/UIKit.h>
#endif
#include <dispatch/dispatch.h>
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstring>
#include <deque>
#include <memory>
#include <mutex>
#include <vector>

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

// True when the PCM holds a sample above a faint noise floor (about -54 dBFS), i.e. something a person hears.
bool audible(std::span<const std::byte> pcm) {
    const std::size_t samples = pcm.size() / 2;
    for (std::size_t i = 0; i < samples; ++i) {
        const auto lo = static_cast<std::uint8_t>(pcm[i * 2]), hi = static_cast<std::uint8_t>(pcm[i * 2 + 1]);
        const auto sample = static_cast<std::int16_t>(static_cast<std::uint16_t>(lo | (hi << 8)));
        if (sample > 64 || sample < -64) return true;
    }
    return false;
}

void note_audible();

// What the device callback touches. Shared with the control queue so a stream can be destroyed while its
// queue is still being disposed.
struct Core {
    AudioQueueRef queue{};
    AudioQueueBufferRef buffers[queue_buffers]{};
    UInt32 buffer_bytes{};
    std::mutex mutex;
    std::deque<std::byte> ring;
    std::atomic<std::uint64_t> played{0};
    bool primed{false};      // control queue only
    bool running{false};     // control queue only: started and not paused
    bool registered{false};  // control queue only: listed in the session's streams

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
        if (audible(std::span(static_cast<const std::byte*>(buffer->mAudioData), copied))) note_audible();
        std::memset(static_cast<char*>(buffer->mAudioData) + copied, 0, buffer_bytes - copied);
        buffer->mAudioDataByteSize = buffer_bytes;
        AudioQueueEnqueueBuffer(queue, buffer, 0, nullptr);
    }
};

// The audio session's mixing policy (iOS). The session mixes with other apps' audio, and only while something
// audible is playing (a voice prompt, a start-up sound) does it lower them, up to `prompt_tail` after the last
// audible sample. Other apps follow a change of ducking only when the session is activated again, so a change
// pauses the running queues, deactivates, sets the new options, reactivates and restarts them; the queues keep
// their buffers, so nothing is lost. All of it runs on the control queue except note_audible, which any thread
// (guest, device callback) may call.
#if TARGET_OS_IOS
constexpr std::chrono::milliseconds prompt_tail{1000};

std::int64_t now_ns() {
    return std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now().time_since_epoch()).count();
}

struct Session {
    std::atomic<bool> duck_enabled{true};
    std::atomic<bool> background{false};
    std::atomic<std::int64_t> last_audible{0};
    std::atomic<bool> prompt{false};  // a prompt is under way; its check is scheduled on the control queue
    // Control queue only.
    bool active{false};
    bool ducking{false};
    std::vector<Core*> streams;

    static Session& get() {
        static Session* session = [] {
            auto* created = new Session;
            NSNotificationCenter* center = NSNotificationCenter.defaultCenter;
            // A suspended app cannot lift its ducking, so it does not duck while in the background.
            [center addObserverForName:UIApplicationDidEnterBackgroundNotification object:nil queue:nil
                            usingBlock:^(NSNotification*) {
                                created->background.store(true);
                                dispatch_async(control_queue(), ^{ if (created->active) created->follow(); });
                            }];
            [center addObserverForName:UIApplicationWillEnterForegroundNotification object:nil queue:nil
                            usingBlock:^(NSNotification*) {
                                created->background.store(false);
                                dispatch_async(control_queue(), ^{ if (created->active) created->follow(); });
                            }];
            return created;
        }();
        return *session;
    }

    bool want_duck() const { return prompt.load() && duck_enabled.load() && !background.load(); }

    void apply(bool duck) {
        AVAudioSession* session = [AVAudioSession sharedInstance];
        if (active) {
            for (Core* core : streams)
                if (core->running) AudioQueuePause(core->queue);
            [session setActive:NO withOptions:duck ? AVAudioSessionSetActiveOptions{0} : AVAudioSessionSetActiveOptionNotifyOthersOnDeactivation
                     error:nil];
        }
        const AVAudioSessionCategoryOptions options = duck
            ? AVAudioSessionCategoryOptionDuckOthers | AVAudioSessionCategoryOptionInterruptSpokenAudioAndMixWithOthers
            : AVAudioSessionCategoryOptionMixWithOthers;
        [session setCategory:AVAudioSessionCategoryPlayback mode:AVAudioSessionModeVoicePrompt options:options error:nil];
        active = [session setActive:YES error:nil];
        ducking = duck;
        for (Core* core : streams)
            if (core->running) AudioQueueStart(core->queue, nullptr);
    }
    // Brings the session to the policy's current answer.
    void follow() {
        const bool duck = want_duck();
        if (!active || duck != ducking) apply(duck);
    }
    void begin() {
        follow();
        schedule_check(prompt_tail);
    }
    void schedule_check(std::chrono::nanoseconds delay) {
        dispatch_after(dispatch_time(DISPATCH_TIME_NOW, delay.count()), control_queue(), ^{ check(); });
    }
    void check() {
        const std::int64_t last = last_audible.load();
        const std::int64_t quiet = now_ns() - last;
        if (quiet < std::chrono::nanoseconds(prompt_tail).count()) {
            schedule_check(std::chrono::nanoseconds(std::chrono::nanoseconds(prompt_tail).count() - quiet));
            return;
        }
        prompt.store(false);
        if (last_audible.load() != last) {
            // Sound came in meanwhile. Whoever raised the flag first again owns the prompt and its check.
            if (prompt.exchange(true)) return;
            schedule_check(prompt_tail);
            return;
        }
        if (active && ducking) apply(false);
    }
};
#endif

void note_audible() {
#if TARGET_OS_IOS
    Session& session = Session::get();
    session.last_audible.store(now_ns());
    if (!session.prompt.exchange(true)) dispatch_async(control_queue(), ^{ Session::get().begin(); });
#endif
}

// Control queue only: the session restarts these when it changes.
void session_attach(Core& core) {
#if TARGET_OS_IOS
    Session& session = Session::get();
    if (!core.registered) { session.streams.push_back(&core); core.registered = true; }
    if (!session.active) session.apply(session.want_duck());
#else
    (void)core;
#endif
}
void session_detach(Core& core) {
#if TARGET_OS_IOS
    if (!core.registered) return;
    auto& streams = Session::get().streams;
    streams.erase(std::remove(streams.begin(), streams.end(), &core), streams.end());
    core.registered = false;
#else
    (void)core;
#endif
}

class QueueStream final : public AudioStream {
public:
    QueueStream(const PcmFormat& format) : format_(format), core_(std::make_shared<Core>()) {}
    ~QueueStream() override {
        if (core_->queue == nullptr) return;
        std::shared_ptr<Core> core = core_;
        // Disposing waits for a running callback; the block keeps the core alive until then.
        dispatch_async(control_queue(), ^{
            session_detach(*core);
            AudioQueueDispose(core->queue, true);
        });
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
        {
            std::lock_guard lock(core_->mutex);
            if (core_->ring.size() + pcm.size() > ring_limit) return false;
            core_->ring.insert(core_->ring.end(), pcm.begin(), pcm.end());
        }
        // Seen here, before the device reaches it, other audio can be lowered in time for the first word.
        if (audible(pcm)) note_audible();
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
                session_attach(*core);
                if (!core->primed) {
                    for (UInt32 i = 0; i < queue_buffers; ++i) core->fill_and_enqueue(core->buffers[i]);
                    core->primed = true;
                }
                AudioQueueStart(core->queue, nullptr);
                core->running = true;
            });
        } else {
            dispatch_async(control_queue(), ^{
                AudioQueuePause(core->queue);
                core->running = false;
            });
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

void AppleAudioSink::set_duck_others(bool enabled) {
#if TARGET_OS_IOS
    Session::get().duck_enabled.store(enabled);
    dispatch_async(control_queue(), ^{
        Session& session = Session::get();
        if (session.active) session.follow();
    });
#else
    (void)enabled;
#endif
}
}
