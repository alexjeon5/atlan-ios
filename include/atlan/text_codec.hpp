// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include <optional>
#include <string>
#include <string_view>

namespace atlan::text {
// How malformed or unmappable input is handled.
enum class Policy : unsigned char {
    strict,   // fail (std::nullopt) on the first bad sequence
    replace,  // U+FFFD when decoding, '?' when encoding to CP949
};

// UTF-8 <-> UTF-16. Decoding rejects overlong forms, surrogates, and values
// above U+10FFFF. Lone UTF-16 surrogates are bad input.
[[nodiscard]] std::optional<std::u16string> utf8_to_utf16(std::string_view utf8, Policy policy = Policy::strict);
[[nodiscard]] std::optional<std::string> utf16_to_utf8(std::u16string_view utf16, Policy policy = Policy::strict);

// Korean EUC-KR extended (CP949 / UHC): ASCII, plus double-byte codes with lead
// 0x81..0xFE and trail 0x41..0xFE. Characters outside the BMP are unmappable.
[[nodiscard]] std::optional<std::u16string> cp949_to_utf16(std::string_view cp949, Policy policy = Policy::strict);
[[nodiscard]] std::optional<std::string> utf16_to_cp949(std::u16string_view utf16, Policy policy = Policy::strict);
[[nodiscard]] std::optional<std::string> cp949_to_utf8(std::string_view cp949, Policy policy = Policy::strict);
[[nodiscard]] std::optional<std::string> utf8_to_cp949(std::string_view utf8, Policy policy = Policy::strict);
}
