// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include "atlan/native_functions.hpp"
#include <cstdint>
#include <functional>
#include <optional>
#include <vector>

// Host versions of the app's text conversion between Korean multibyte text (CP949) and UTF-16LE code units, for
// builds that convert through a general-purpose iconv per call. The rules are those of a converter that stops at
// the first sequence it cannot convert or that does not fit: what was converted before it stays, and the result
// counts it.
namespace atlan::shell::charset {
// Reads one guest byte or code unit at an offset from the start of the input; nullopt: unreadable.
using ByteAt = std::function<std::optional<std::uint8_t>(std::uint32_t offset)>;
using UnitAt = std::function<std::optional<std::uint16_t>(std::uint32_t offset)>;
// Receives one converted code unit or byte; false: unwritable.
using PutUnit = std::function<bool(std::uint16_t unit)>;
using PutByte = std::function<bool(std::uint8_t byte)>;

// The code point of one CP949 character starting with `lead` (and `trail` for a double-byte lead), or nullopt.
[[nodiscard]] std::optional<char16_t> decode(std::uint8_t lead, std::uint8_t trail) noexcept;
// The CP949 bytes of a code point: (lead << 8 | trail), or the byte itself below 0x80; nullopt if unmappable.
[[nodiscard]] std::optional<std::uint16_t> encode(char16_t code_point);

// Converts at most `in_bytes` bytes into at most `out_bytes` bytes of UTF-16LE. nullopt: unreadable input or
// unwritable output. Otherwise the output bytes left over.
[[nodiscard]] std::optional<std::uint32_t> to_wide(const ByteAt& in, std::uint32_t in_bytes, const PutUnit& put, std::uint32_t out_bytes);
// Converts at most `in_bytes` bytes of UTF-16LE (whole code units) into at most `out_bytes` bytes of CP949.
[[nodiscard]] std::optional<std::uint32_t> to_multibyte(const UnitAt& in, std::uint32_t in_bytes, const PutByte& put, std::uint32_t out_bytes);

// Table-driven conversion of other builds: each byte up to 0x80 stands for itself, any other byte starts a two-byte
// code that becomes its code point, or '?' if it has none. It stops at a NUL lead byte, when `in_bytes` (counted
// down by 1 or 2, so an odd count runs on to a NUL) reaches 0, or after `room` units; then a terminating 0 follows if
// room is left. nullopt: unreadable input.
struct TableConversion {
    std::vector<char16_t> units;  // without the terminator
    bool terminator{};
    std::uint32_t bytes_read{};   // how far the input was read
    bool outside_table{};         // met a code beyond the end of the table; the rest is not valid
};
[[nodiscard]] std::optional<TableConversion> table_to_wide(const ByteAt& in, std::uint32_t in_bytes, std::uint32_t room);

// Variants of the conversion entry points (members "multibyte_to_wide" and "wide_to_multibyte", located by a
// signature file); at most one is installed. `exhaustive`: the converter that stops is compared on every double-byte
// code and every code unit one by one, which takes far longer than the default check; for checking a new build once.
[[nodiscard]] std::vector<NativeSet> conversion_sets(bool exhaustive = false);
}
