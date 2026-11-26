// SPDX-License-Identifier: GPL-2.0-or-later
#include "opensles_audio_sink.hpp"
#include <SLES/OpenSLES.h>
#include <SLES/OpenSLES_Android.h>
#include <algorithm>
#include <array>
#include <atomic>
#include <cstring>
#include <deque>
#include <vector>

namespace atlan {
namespace {
constexpr unsigned buffers_in_flight = 3;
std::string sl_error(const char* what, SLresult result) { return std::string(what) + " failed (" + std::to_string(result) + ")"; }
}

struct OpenSlesAudioSink::Engine {
    SLObjectItf object{};
    SLEngineItf engine{};
    SLObjectItf mix{};
    ~Engine() {
        if (mix) (*mix)->Destroy(mix);
        if (object) (*object)->Destroy(object);
    }
};

namespace {
class OpenSlesStream final : public AudioStream {
public:
    OpenSlesStream(std::shared_ptr<OpenSlesAudioSink::Engine> engine, PcmFormat format)
        : engine_(std::move(engine)), format_(format) {
        // 20 ms per chunk, whole frames.
        chunk_bytes_ = std::max<std::size_t>(format.frame_bytes(), format.sample_rate / 50 * format.frame_bytes());
        for (auto& b : buffers_) b.data.resize(chunk_bytes_);
    }
    ~OpenSlesStream() override {
        if (player_) (*player_)->Destroy(player_);  // no callback runs after Destroy returns
    }

    bool create(std::string& error) {
        auto& e = *engine_;
        SLDataLocator_AndroidSimpleBufferQueue queue_locator{SL_DATALOCATOR_ANDROIDSIMPLEBUFFERQUEUE, buffers_in_flight};
        SLDataFormat_PCM pcm{SL_DATAFORMAT_PCM, format_.channels, format_.sample_rate * 1000, SL_PCMSAMPLEFORMAT_FIXED_16,
                             SL_PCMSAMPLEFORMAT_FIXED_16,
                             format_.channels == 1 ? SL_SPEAKER_FRONT_CENTER : (SL_SPEAKER_FRONT_LEFT | SL_SPEAKER_FRONT_RIGHT),
                             SL_BYTEORDER_LITTLEENDIAN};
        SLDataSource source{&queue_locator, &pcm};
        SLDataLocator_OutputMix mix_locator{SL_DATALOCATOR_OUTPUTMIX, e.mix};
        SLDataSink sink{&mix_locator, nullptr};
        const SLInterfaceID ids[] = {SL_IID_ANDROIDSIMPLEBUFFERQUEUE};
        const SLboolean required[] = {SL_BOOLEAN_TRUE};
        SLresult r = (*e.engine)->CreateAudioPlayer(e.engine, &player_, &source, &sink, 1, ids, required);
        if (r != SL_RESULT_SUCCESS) { player_ = nullptr; error = sl_error("CreateAudioPlayer", r); return false; }
        if ((r = (*player_)->Realize(player_, SL_BOOLEAN_FALSE)) != SL_RESULT_SUCCESS) { error = sl_error("Realize(player)", r); return false; }
        if ((r = (*player_)->GetInterface(player_, SL_IID_PLAY, &play_)) != SL_RESULT_SUCCESS) { error = sl_error("GetInterface(PLAY)", r); return false; }
        if ((r = (*player_)->GetInterface(player_, SL_IID_ANDROIDSIMPLEBUFFERQUEUE, &queue_)) != SL_RESULT_SUCCESS) {
            error = sl_error("GetInterface(BUFFERQUEUE)", r);
            return false;
        }
        if ((r = (*queue_)->RegisterCallback(queue_, &OpenSlesStream::done, this)) != SL_RESULT_SUCCESS) {
            error = sl_error("RegisterCallback", r);
            return false;
        }
        return true;
    }

    bool append(std::span<const std::byte> pcm) override {
        std::lock_guard lock(lock_);
        ring_.insert(ring_.end(), pcm.begin(), pcm.end());
        return true;
    }
    void flush() override {
        std::lock_guard lock(lock_);
        ring_.clear();
    }
    void set_playing(bool playing) override {
        {
            std::lock_guard lock(lock_);
            if (playing == playing_) return;
            playing_ = playing;
            // Prime the queue; afterwards each finished buffer refills itself.
            if (playing)
                while (queued_ < buffers_in_flight)
                    if (!enqueue_locked()) break;
        }
        // Outside the lock: the player may wait for a running callback, which takes it.
        (*play_)->SetPlayState(play_, playing ? SL_PLAYSTATE_PLAYING : SL_PLAYSTATE_PAUSED);
    }
    void set_gain(float gain) override { gain_.store(std::clamp(gain, 0.0F, 1.0F), std::memory_order_relaxed); }
    std::uint64_t played_bytes() const override { return played_.load(std::memory_order_acquire); }

private:
    struct Buffer {
        std::vector<std::byte> data;
        std::size_t pcm{};  // bytes of guest PCM at its start; the rest is silence
    };
    // The device finished the oldest buffer (OpenSL ES thread).
    static void done(SLAndroidSimpleBufferQueueItf, void* context) {
        auto& self = *static_cast<OpenSlesStream*>(context);
        std::lock_guard lock(self.lock_);
        auto& finished = self.buffers_[self.head_];
        self.played_.fetch_add(finished.pcm, std::memory_order_acq_rel);
        finished.pcm = 0;
        self.head_ = (self.head_ + 1) % buffers_in_flight;
        --self.queued_;
        if (self.playing_) self.enqueue_locked();
    }
    bool enqueue_locked() {
        auto& b = buffers_[(head_ + queued_) % buffers_in_flight];
        const std::size_t take = std::min(chunk_bytes_, ring_.size() / format_.frame_bytes() * format_.frame_bytes());
        std::copy_n(ring_.begin(), take, b.data.begin());
        ring_.erase(ring_.begin(), ring_.begin() + static_cast<std::ptrdiff_t>(take));
        std::fill(b.data.begin() + static_cast<std::ptrdiff_t>(take), b.data.end(), std::byte{0});
        b.pcm = take;
        const float gain = gain_.load(std::memory_order_relaxed);
        if (gain < 1.0F) {
            for (std::size_t i = 0; i + 1 < take; i += 2) {
                std::int16_t sample;
                std::memcpy(&sample, b.data.data() + i, 2);
                sample = static_cast<std::int16_t>(static_cast<float>(sample) * gain);
                std::memcpy(b.data.data() + i, &sample, 2);
            }
        }
        if ((*queue_)->Enqueue(queue_, b.data.data(), static_cast<SLuint32>(b.data.size())) != SL_RESULT_SUCCESS) {
            ring_.insert(ring_.begin(), b.data.begin(), b.data.begin() + static_cast<std::ptrdiff_t>(take));  // keep it for later
            b.pcm = 0;
            return false;
        }
        ++queued_;
        return true;
    }

    std::shared_ptr<OpenSlesAudioSink::Engine> engine_;
    PcmFormat format_;
    SLObjectItf player_{};
    SLPlayItf play_{};
    SLAndroidSimpleBufferQueueItf queue_{};
    std::mutex lock_;  // ring, buffers, queue counters, playing
    std::deque<std::byte> ring_;
    std::array<Buffer, buffers_in_flight> buffers_;
    std::size_t chunk_bytes_{};
    unsigned head_{}, queued_{};
    bool playing_{};
    std::atomic<float> gain_{1.0F};
    std::atomic<std::uint64_t> played_{0};
};
}

OpenSlesAudioSink::OpenSlesAudioSink() = default;
OpenSlesAudioSink::~OpenSlesAudioSink() = default;

std::unique_ptr<AudioStream> OpenSlesAudioSink::open(const PcmFormat& format, std::string& error) {
    if (format.channels != 1 && format.channels != 2) { error = "unsupported channel count"; return nullptr; }
    std::shared_ptr<Engine> engine;
    {
        std::lock_guard lock(lock_);
        if (!engine_) {
            auto e = std::make_shared<Engine>();
            const SLEngineOption options[] = {{SL_ENGINEOPTION_THREADSAFE, SL_BOOLEAN_TRUE}};
            SLresult r = slCreateEngine(&e->object, 1, options, 0, nullptr, nullptr);
            if (r != SL_RESULT_SUCCESS) { e->object = nullptr; error = sl_error("slCreateEngine", r); return nullptr; }
            if ((r = (*e->object)->Realize(e->object, SL_BOOLEAN_FALSE)) != SL_RESULT_SUCCESS) { error = sl_error("Realize(engine)", r); return nullptr; }
            if ((r = (*e->object)->GetInterface(e->object, SL_IID_ENGINE, &e->engine)) != SL_RESULT_SUCCESS) { error = sl_error("GetInterface(ENGINE)", r); return nullptr; }
            if ((r = (*e->engine)->CreateOutputMix(e->engine, &e->mix, 0, nullptr, nullptr)) != SL_RESULT_SUCCESS) {
                e->mix = nullptr;
                error = sl_error("CreateOutputMix", r);
                return nullptr;
            }
            if ((r = (*e->mix)->Realize(e->mix, SL_BOOLEAN_FALSE)) != SL_RESULT_SUCCESS) { error = sl_error("Realize(mix)", r); return nullptr; }
            engine_ = std::move(e);
        }
        engine = engine_;
    }
    auto stream = std::make_unique<OpenSlesStream>(std::move(engine), format);
    if (!stream->create(error)) return nullptr;
    return stream;
}
}
