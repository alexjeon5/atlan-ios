// SPDX-License-Identifier: GPL-2.0-or-later
#include "atlan/audio.hpp"

namespace atlan {
class RecordingStream final : public AudioStream {
public:
    RecordingStream(RecordingAudioSink& sink, std::size_t index) : sink_(sink), index_(index) {}
    ~RecordingStream() override {
        std::lock_guard lock(sink_.mutex_);
        sink_.records_[index_].open = false;
        sink_.records_[index_].playing = false;
    }
    bool append(std::span<const std::byte> pcm) override {
        std::lock_guard lock(sink_.mutex_);
        auto& r = sink_.records_[index_];
        r.data.insert(r.data.end(), pcm.begin(), pcm.end());
        r.appended += pcm.size();
        if (sink_.auto_consume_ && r.playing) r.played = r.appended;
        return true;
    }
    void flush() override {
        std::lock_guard lock(sink_.mutex_);
        auto& r = sink_.records_[index_];
        ++r.flushes;
        r.appended = r.played;  // unplayed data is dropped; the counters stay consistent
        const auto drop = r.data.size() - std::min<std::size_t>(r.data.size(), static_cast<std::size_t>(r.played));
        r.data.resize(r.data.size() - drop);
    }
    void set_playing(bool playing) override {
        std::lock_guard lock(sink_.mutex_);
        auto& r = sink_.records_[index_];
        r.playing = playing;
        if (playing && sink_.auto_consume_) r.played = r.appended;
    }
    void set_gain(float gain) override {
        std::lock_guard lock(sink_.mutex_);
        sink_.records_[index_].gain = gain;
    }
    std::uint64_t played_bytes() const override {
        std::lock_guard lock(sink_.mutex_);
        return sink_.records_[index_].played;
    }
private:
    RecordingAudioSink& sink_;
    std::size_t index_;
};

std::unique_ptr<AudioStream> RecordingAudioSink::open(const PcmFormat& format, std::string& error) {
    std::lock_guard lock(mutex_);
    if (!fail_.empty()) { error = std::move(fail_); fail_.clear(); return nullptr; }
    records_.emplace_back();
    records_.back().format = format;
    return std::make_unique<RecordingStream>(*this, records_.size() - 1);
}
std::uint64_t RecordingAudioSink::consume(std::size_t index, std::uint64_t bytes) {
    std::lock_guard lock(mutex_);
    auto& r = records_.at(index);
    if (!r.playing) return 0;
    const auto n = std::min<std::uint64_t>(bytes, r.appended - r.played);
    r.played += n;
    return n;
}
RecordingAudioSink::Record RecordingAudioSink::record(std::size_t index) const {
    std::lock_guard lock(mutex_);
    return records_.at(index);
}
std::size_t RecordingAudioSink::stream_count() const {
    std::lock_guard lock(mutex_);
    return records_.size();
}
void RecordingAudioSink::fail_next_open(std::string reason) {
    std::lock_guard lock(mutex_);
    fail_ = std::move(reason);
}
}
