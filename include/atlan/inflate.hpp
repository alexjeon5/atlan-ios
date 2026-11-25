// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include <array>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <span>

namespace atlan {
[[nodiscard]] std::uint32_t adler32(std::uint32_t adler, std::span<const std::uint8_t> data) noexcept;
[[nodiscard]] std::uint32_t crc32(std::uint32_t crc, std::span<const std::uint8_t> data) noexcept;

// Resumable DEFLATE (RFC 1951) decoder with optional zlib framing (RFC 1950). Input and output arrive in
// pieces of any size. Like zlib, a call consumes only the input bytes it needed when it stops for output
// space or at the end of the stream, and all of them when it needs more input.
class Inflater {
public:
    enum class Stop : std::uint8_t {
        input,   // every input byte was consumed; more is needed
        output,  // the output is full
        end,     // the stream ended (and its checksum matched)
        error,   // invalid data; every later call reports the same
        dictionary  // the zlib header asks for a preset dictionary, which is not supported
    };
    struct Progress {
        std::size_t consumed{}, produced{};
        Stop stop{Stop::input};
    };
    explicit Inflater(bool zlib_framing = true);
    ~Inflater();
    Inflater(Inflater&&) noexcept;
    Inflater& operator=(Inflater&&) noexcept;
    [[nodiscard]] Progress run(std::span<const std::uint8_t> input, std::span<std::uint8_t> output) noexcept;
    // Adler-32 of everything produced so far.
    [[nodiscard]] std::uint32_t checksum() const noexcept;
    void reset() noexcept;

private:
    struct State;
    std::unique_ptr<State> state_;
};
}
