// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

// A small strict JSON (RFC 8259) reader for short documents the host fetches or keeps, e.g. a list the app downloads.
// Numbers are kept as doubles; duplicate object keys, trailing commas, comments and nesting deeper than 32 are errors.
namespace atlan::shell {
struct JsonValue {
    enum class Kind : unsigned char { null, boolean, number, string, array, object };
    Kind kind{Kind::null};
    bool boolean{};
    double number{};
    std::string string;
    std::vector<JsonValue> array;
    std::map<std::string, JsonValue, std::less<>> object;

    // The member `key` of an object, or nullptr (also when this is no object).
    [[nodiscard]] const JsonValue* find(std::string_view key) const;
};

// The document in `text`, or nullopt with `why` naming the first error and its byte offset.
[[nodiscard]] std::optional<JsonValue> parse_json(std::string_view text, std::string& why);
}
