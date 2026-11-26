// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include "atlan/audio.hpp"
#include <cstddef>
#include <filesystem>
#include <memory>
#include <optional>
#include <span>
#include <string>

namespace atlan {
// The PCM samples of a RIFF/WAVE file: 16-bit integer PCM (format tag 1, or WAVE_FORMAT_EXTENSIBLE with the PCM
// sub-format), any channel count and integer rate. `samples` points into the bytes given to parse_wav.
struct WavPcm {
    PcmFormat format;
    std::span<const std::byte> samples;
};
// nullopt with `error` set when the bytes are no WAVE file or hold another sample format. A data chunk that runs
// past the end of the file (a truncated recording) is cut to whole frames.
[[nodiscard]] std::optional<WavPcm> parse_wav(std::span<const std::byte> bytes, std::string& error);

// Plays a WAVE file once from its start, as Android's MediaPlayer does for an app's sound file. The returned stream
// plays until it runs out of samples; destroying it stops the sound. nullptr with `error` set when the file cannot be
// read, is no supported WAVE file or the sink refuses it.
[[nodiscard]] std::unique_ptr<AudioStream> play_wav_file(AudioSink& sink, const std::filesystem::path& path, std::string& error);
}
