// SPDX-License-Identifier: GPL-2.0-or-later
#include "adapters/atlan/verified_versions.hpp"
#include "shell/json.hpp"
#include <algorithm>
#include <initializer_list>

namespace atlan::shell {
namespace {
bool only_keys(const JsonValue& object, std::initializer_list<std::string_view> keys, std::string& why, std::string_view where) {
    for (const auto& [key, value] : object.object) {
        (void)value;
        if (std::find(keys.begin(), keys.end(), key) == keys.end()) { why = std::string(where) + " has an unknown key \"" + key + "\""; return false; }
    }
    return true;
}
const std::string* text(const JsonValue& object, std::string_view key) {
    const auto* value = object.find(key);
    return value && value->kind == JsonValue::Kind::string && !value->string.empty() && value->string.size() <= 64 ? &value->string : nullptr;
}
bool is_digest(const std::string& value) {
    return value.size() == 64 && std::all_of(value.begin(), value.end(), [](char c) { return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f'); });
}
bool is_date(const std::string& value) {
    if (value.size() != 10 || value[4] != '-' || value[7] != '-') return false;
    for (std::size_t i = 0; i < value.size(); ++i)
        if (i != 4 && i != 7 && (value[i] < '0' || value[i] > '9')) return false;
    return true;
}
}

const VerifiedBuild* VerifiedList::find(std::string_view sha256) const noexcept {
    const auto found = std::find_if(builds.begin(), builds.end(), [&](const VerifiedBuild& build) { return build.sha256 == sha256; });
    return found == builds.end() ? nullptr : &*found;
}

std::optional<VerifiedList> parse_verified_list(std::string_view document, std::string& why) {
    if (document.size() > 256 * 1024) { why = "list is too large"; return std::nullopt; }
    const auto root = parse_json(document, why);
    if (!root) return std::nullopt;
    if (root->kind != JsonValue::Kind::object) { why = "list is not an object"; return std::nullopt; }
    if (!only_keys(*root, {"format", "updated", "builds"}, why, "list")) return std::nullopt;
    const auto* format = root->find("format");
    if (!format || format->kind != JsonValue::Kind::number || format->number != 1) { why = "unsupported list format"; return std::nullopt; }
    VerifiedList list;
    const auto* updated = text(*root, "updated");
    if (!updated || !is_date(*updated)) { why = "list date is missing or not YYYY-MM-DD"; return std::nullopt; }
    list.updated = *updated;
    const auto* builds = root->find("builds");
    if (!builds || builds->kind != JsonValue::Kind::array) { why = "list has no builds array"; return std::nullopt; }
    for (std::size_t i = 0; i < builds->array.size(); ++i) {
        const auto& entry = builds->array[i];
        const std::string where = "build " + std::to_string(i);
        if (entry.kind != JsonValue::Kind::object) { why = where + " is not an object"; return std::nullopt; }
        if (!only_keys(entry, {"name", "version", "sha256"}, why, where)) return std::nullopt;
        const auto *name = text(entry, "name"), *version = text(entry, "version"), *digest = text(entry, "sha256");
        if (!name || !version) { why = where + " needs a name and a version"; return std::nullopt; }
        if (!digest || !is_digest(*digest)) { why = where + " needs a sha256 of 64 lowercase hex digits"; return std::nullopt; }
        if (list.find(*digest)) { why = where + " repeats a digest"; return std::nullopt; }
        list.builds.push_back({*name, *version, *digest});
    }
    return list;
}
}
