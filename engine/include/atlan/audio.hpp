// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include <cstddef>
#include <cstdint>
#include <memory>
#include <mutex>
#include <span>
#include <string>
#include <vector>

namespace atlan {
// Interleaved signed 16-bit little-endian PCM, the only format the guest has been
// observed to need. Other formats are refused explicitly by the OpenSL ES layer.
struct PcmFormat {
    std::uint32_t sample_rate{};
    std::uint16_t channels{};
    [[nodiscard]] std::size_t frame_bytes() const noexcept { return std::size_t{channels} * 2; }
    friend bool operator==(const PcmFormat&, const PcmFormat&) = default;
};

// One playback stream of a host audio backend. All methods are called from the
// guest thread; played_bytes() may advance on the backend's render thread, so
// implementations must make it safe to read concurrently (it is monotonic).
class AudioStream {
public:
    virtual ~AudioStream() = default;
    // Queues PCM for playback. The sink must accept it all or refuse it all.
    [[nodiscard]] virtual bool append(std::span<const std::byte> pcm) = 0;
    // Discards queued data that has not been played yet. played_bytes() is unchanged.
    virtual void flush() = 0;
    // The stream renders queued data only while playing.
    virtual void set_playing(bool playing) = 0;
    // Linear gain in [0, 1], applied by the sink; muted streams render silence.
    virtual void set_gain(float gain) = 0;
    // Total bytes consumed by the device since the stream opened (monotonic).
    [[nodiscard]] virtual std::uint64_t played_bytes() const = 0;
};

class AudioSink {
public:
    virtual ~AudioSink() = default;
    // Opens a stream. nullptr with `error` set when the device cannot do it.
    [[nodiscard]] virtual std::unique_ptr<AudioStream> open(const PcmFormat& format, std::string& error) = 0;
};

// Portable sink for tests and headless tools: records every appended byte and
// consumes data only when told to (consume) or, in auto mode, as soon as it is
// appended while the stream is playing. Thread-safe.
class RecordingAudioSink final : public AudioSink {
public:
    struct Record {
        PcmFormat format{};
        std::vector<std::byte> data;       // everything appended and not flushed before playing
        std::uint64_t appended{}, played{};
        bool playing{}, open{true};
        float gain{1.0F};
        unsigned flushes{};
    };
    explicit RecordingAudioSink(bool auto_consume = false) : auto_consume_(auto_consume) {}
    std::unique_ptr<AudioStream> open(const PcmFormat& format, std::string& error) override;
    // Marks up to `bytes` of pending data of stream `index` as played; returns the amount.
    std::uint64_t consume(std::size_t index, std::uint64_t bytes);
    [[nodiscard]] Record record(std::size_t index) const;
    [[nodiscard]] std::size_t stream_count() const;
    void fail_next_open(std::string reason);
private:
    friend class RecordingStream;
    mutable std::mutex mutex_;
    std::vector<Record> records_;
    bool auto_consume_;
    std::string fail_;
};
}
