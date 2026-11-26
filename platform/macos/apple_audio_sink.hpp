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
// On iOS the first stream that plays activates a voice-prompt audio session that mixes with other apps.
// While a prompt plays (one appended buffer audible for at least 0.7 s, so not a button click) and for a
// short tail after it, the session lowers other apps' audio, unless set_duck_others(false). Starting, pausing and disposing queues and session changes run on a serial
// dispatch queue so the guest thread never waits for the device.
class AppleAudioSink final : public AudioSink {
public:
    std::unique_ptr<AudioStream> open(const PcmFormat& format, std::string& error) override;
    // Whether audible output lowers other apps' audio (iOS; on by default). Applies to every sink at once.
    static void set_duck_others(bool enabled);
};
}
