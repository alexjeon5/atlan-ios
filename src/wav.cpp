// SPDX-License-Identifier: GPL-2.0-or-later
#include "atlan/wav.hpp"
#include <algorithm>
#include <cstdint>
#include <cstring>
#include <fstream>
#include <iterator>
#include <vector>

namespace atlan {
namespace {
std::uint32_t le32(std::span<const std::byte> b, std::size_t at) {
    return std::to_integer<std::uint32_t>(b[at]) | std::to_integer<std::uint32_t>(b[at + 1]) << 8 |
           std::to_integer<std::uint32_t>(b[at + 2]) << 16 | std::to_integer<std::uint32_t>(b[at + 3]) << 24;
}
std::uint16_t le16(std::span<const std::byte> b, std::size_t at) {
    return static_cast<std::uint16_t>(std::to_integer<unsigned>(b[at]) | std::to_integer<unsigned>(b[at + 1]) << 8);
}
bool tag(std::span<const std::byte> b, std::size_t at, const char (&name)[5]) {
    return std::memcmp(b.data() + at, name, 4) == 0;
}
constexpr std::uint16_t format_pcm = 1, format_extensible = 0xfffe;
}

std::optional<WavPcm> parse_wav(std::span<const std::byte> bytes, std::string& error) {
    if (bytes.size() < 12 || !tag(bytes, 0, "RIFF") || !tag(bytes, 8, "WAVE")) { error = "not a RIFF/WAVE file"; return std::nullopt; }
    std::optional<PcmFormat> format;
    std::size_t at = 12;
    while (at + 8 <= bytes.size()) {
        const std::size_t size = le32(bytes, at + 4);
        const std::size_t body = at + 8;
        const std::size_t available = std::min(size, bytes.size() - body);
        if (tag(bytes, at, "fmt ")) {
            if (available < 16) { error = "short fmt chunk"; return std::nullopt; }
            auto kind = le16(bytes, body);
            const auto channels = le16(bytes, body + 2);
            const auto rate = le32(bytes, body + 4);
            const auto bits = le16(bytes, body + 14);
            // WAVE_FORMAT_EXTENSIBLE: the sub-format GUID starts with the plain format tag.
            if (kind == format_extensible && available >= 26) kind = le16(bytes, body + 24);
            if (kind != format_pcm || bits != 16) {
                error = "unsupported WAVE format " + std::to_string(kind) + " with " + std::to_string(bits) + "-bit samples";
                return std::nullopt;
            }
            if (channels == 0 || rate == 0) { error = "WAVE file without channels or rate"; return std::nullopt; }
            format = PcmFormat{rate, channels};
        } else if (tag(bytes, at, "data")) {
            if (!format) { error = "data chunk before fmt chunk"; return std::nullopt; }
            const auto whole = available - available % format->frame_bytes();
            return WavPcm{*format, bytes.subspan(body, whole)};
        }
        at = body + size + (size & 1);  // chunks are padded to even sizes
    }
    error = format ? "no data chunk" : "no fmt chunk";
    return std::nullopt;
}

std::unique_ptr<AudioStream> play_wav_file(AudioSink& sink, const std::filesystem::path& path, std::string& error) {
    std::ifstream input(path, std::ios::binary);
    if (!input) { error = "cannot open " + path.string(); return nullptr; }
    std::vector<char> chars((std::istreambuf_iterator<char>(input)), std::istreambuf_iterator<char>());
    const std::span<const std::byte> bytes(reinterpret_cast<const std::byte*>(chars.data()), chars.size());
    const auto pcm = parse_wav(bytes, error);
    if (!pcm) return nullptr;
    auto stream = sink.open(pcm->format, error);
    if (!stream) return nullptr;
    if (!stream->append(pcm->samples)) { error = "the audio device refused " + std::to_string(pcm->samples.size()) + " bytes"; return nullptr; }
    stream->set_playing(true);
    return stream;
}
}
