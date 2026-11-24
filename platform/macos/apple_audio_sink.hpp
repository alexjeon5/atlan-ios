// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include "atlan/audio.hpp"

namespace atlan {
// AudioQueue-backed AudioSink for macOS and iOS (AudioToolbox exists on both).
// Each stream owns an AudioQueue fed from a locked byte ring; the device callback
// copies queued PCM into small queue buffers and pads with silence when the ring
// is empty. played_bytes() counts real PCM handed to the device buffers, so it
// runs ahead of what is audible by at most the queue depth (three ~20 ms
// buffers). flush() drops the ring; data already in device buffers still plays.
// On iOS opening a stream also configures a ducking, voice-prompt audio session.
class AppleAudioSink final : public AudioSink {
public:
    std::unique_ptr<AudioStream> open(const PcmFormat& format, std::string& error) override;
};
}
