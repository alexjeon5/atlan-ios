// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

namespace atlan::shell {
// Settings text with one `KEY|TYPE=VALUE` entry per line (LF or CRLF), as the guest reads it.
// The value of the first entry named `key`, without surrounding blanks; nullopt if there is none.
[[nodiscard]] std::optional<std::string> ini_value(std::string_view text, std::string_view key);
// The same value as a non-negative decimal integer; nullopt if absent or not a number.
[[nodiscard]] std::optional<std::uint32_t> ini_number(std::string_view text, std::string_view key);
// `text` with the value of every entry named `key` replaced. Other bytes, including line endings, are kept.
// Without such an entry, `KEY|TYPE=VALUE` is appended (type 1: integer, 2: text).
[[nodiscard]] std::string ini_set(std::string text, std::string_view key, std::string_view value, char type = '1');
}
