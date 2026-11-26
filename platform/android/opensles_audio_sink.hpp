// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include "atlan/audio.hpp"
#include <memory>
#include <mutex>
#include <string>

namespace atlan {
// AudioSink on the device's own OpenSL ES (Android). Each stream is an audio player fed from a buffer queue: the
// guest's PCM goes into a ring, the player's callback takes it in fixed chunks (silence when the ring runs dry,
// which is not counted as played) and counts a chunk's PCM as played when the device finishes it.
class OpenSlesAudioSink final : public AudioSink {
public:
    OpenSlesAudioSink();
    ~OpenSlesAudioSink() override;
    OpenSlesAudioSink(const OpenSlesAudioSink&) = delete;
    OpenSlesAudioSink& operator=(const OpenSlesAudioSink&) = delete;
    std::unique_ptr<AudioStream> open(const PcmFormat& format, std::string& error) override;
    struct Engine;
private:
    std::mutex lock_;
    std::shared_ptr<Engine> engine_;  // created on the first open
};
}
