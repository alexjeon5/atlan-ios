// SPDX-License-Identifier: GPL-2.0-or-later
#include "shell/guest_ini.hpp"
#include <cstdint>

namespace atlan::shell {
namespace {
struct Entry { std::size_t value_begin{}, value_end{}; };
// Calls visit(entry) for each line whose key is `key`; stops when visit returns false.
template <class Visit>
void each_entry(std::string_view text, std::string_view key, Visit visit) {
    for (std::size_t line = 0; line < text.size();) {
        auto end = text.find('\n', line);
        if (end == std::string_view::npos) end = text.size();
        auto stop = end;
        while (stop > line && (text[stop - 1] == '\r' || text[stop - 1] == ' ' || text[stop - 1] == '\t')) --stop;
        const auto bar = text.find('|', line);
        const auto equals = text.find('=', line);
        if (bar < stop && equals < stop && bar < equals && text.substr(line, bar - line) == key) {
            auto begin = equals + 1;
            while (begin < stop && (text[begin] == ' ' || text[begin] == '\t')) ++begin;
            if (!visit(Entry{begin, stop})) return;
        }
        line = end + 1;
    }
}
}

std::optional<std::string> ini_value(std::string_view text, std::string_view key) {
    std::optional<std::string> found;
    each_entry(text, key, [&](Entry entry) {
        found = std::string(text.substr(entry.value_begin, entry.value_end - entry.value_begin));
        return false;
    });
    return found;
}

std::optional<std::uint32_t> ini_number(std::string_view text, std::string_view key) {
    const auto value = ini_value(text, key);
    if (!value || value->empty() || value->size() > 9) return std::nullopt;
    std::uint32_t number = 0;
    for (const char c : *value) {
        if (c < '0' || c > '9') return std::nullopt;
        number = number * 10 + static_cast<std::uint32_t>(c - '0');
    }
    return number;
}

std::string ini_set(std::string text, std::string_view key, std::string_view value) {
    std::string out;
    std::size_t copied = 0;
    bool found = false;
    each_entry(text, key, [&](Entry entry) {
        out.append(text, copied, entry.value_begin - copied);
        out.append(value);
        copied = entry.value_end;
        found = true;
        return true;
    });
    if (!found) {
        const bool crlf = text.find("\r\n") != std::string::npos;
        if (!text.empty() && text.back() != '\n') text += crlf ? "\r\n" : "\n";
        text.append(key).append("|1=").append(value).append(crlf ? "\r\n" : "\n");
        return text;
    }
    out.append(text, copied, std::string::npos);
    return out;
}
}
