// SPDX-License-Identifier: GPL-2.0-or-later
#include "atlan/text_codec.hpp"
#include <algorithm>
#include <cstdint>
#include <utility>
#include <vector>

namespace atlan::text {
namespace {
constexpr char16_t replacement = 0xFFFD;
constexpr unsigned lead_first = 0x81, lead_last = 0xFE, trail_first = 0x41, trail_last = 0xFE;
constexpr unsigned trail_count = trail_last - trail_first + 1, lead_count = lead_last - lead_first + 1;

constexpr char16_t forward_table[lead_count][trail_count] = {
#include "cp949_table.inc"
};

struct Reverse {
    std::vector<std::pair<char16_t, std::uint16_t>> entries;  // sorted by code point
    Reverse() {
        for (unsigned lead = 0; lead < lead_count; ++lead)
            for (unsigned trail = 0; trail < trail_count; ++trail)
                if (const char16_t cp = forward_table[lead][trail])
                    entries.emplace_back(cp, static_cast<std::uint16_t>(((lead + lead_first) << 8) | (trail + trail_first)));
        std::sort(entries.begin(), entries.end());
    }
    [[nodiscard]] std::optional<std::uint16_t> find(char16_t cp) const {
        const auto it = std::lower_bound(entries.begin(), entries.end(), cp, [](const auto& e, char16_t v) { return e.first < v; });
        if (it == entries.end() || it->first != cp) return std::nullopt;
        return it->second;
    }
};
const Reverse& reverse() {
    static const Reverse table;
    return table;
}

bool is_high(char32_t c) { return c >= 0xD800 && c <= 0xDBFF; }
bool is_low(char32_t c) { return c >= 0xDC00 && c <= 0xDFFF; }

void append_utf8(std::string& out, char32_t cp) {
    if (cp < 0x80) {
        out += static_cast<char>(cp);
    } else if (cp < 0x800) {
        out += static_cast<char>(0xC0 | (cp >> 6));
        out += static_cast<char>(0x80 | (cp & 0x3F));
    } else if (cp < 0x10000) {
        out += static_cast<char>(0xE0 | (cp >> 12));
        out += static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
        out += static_cast<char>(0x80 | (cp & 0x3F));
    } else {
        out += static_cast<char>(0xF0 | (cp >> 18));
        out += static_cast<char>(0x80 | ((cp >> 12) & 0x3F));
        out += static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
        out += static_cast<char>(0x80 | (cp & 0x3F));
    }
}
void append_utf16(std::u16string& out, char32_t cp) {
    if (cp < 0x10000) {
        out += static_cast<char16_t>(cp);
    } else {
        cp -= 0x10000;
        out += static_cast<char16_t>(0xD800 + (cp >> 10));
        out += static_cast<char16_t>(0xDC00 + (cp & 0x3FF));
    }
}
}

std::optional<std::u16string> utf8_to_utf16(std::string_view in, Policy policy) {
    std::u16string out;
    out.reserve(in.size());
    for (std::size_t i = 0; i < in.size();) {
        const auto b = static_cast<unsigned char>(in[i]);
        std::size_t length = 0;
        char32_t cp = 0, minimum = 0;
        if (b < 0x80) { length = 1; cp = b; }
        else if (b >= 0xC2 && b <= 0xDF) { length = 2; cp = b & 0x1F; minimum = 0x80; }
        else if (b >= 0xE0 && b <= 0xEF) { length = 3; cp = b & 0x0F; minimum = 0x800; }
        else if (b >= 0xF0 && b <= 0xF4) { length = 4; cp = b & 0x07; minimum = 0x10000; }
        bool ok = length != 0 && i + length <= in.size();
        for (std::size_t k = 1; ok && k < length; ++k) {
            const auto c = static_cast<unsigned char>(in[i + k]);
            ok = (c & 0xC0) == 0x80;
            cp = (cp << 6) | (c & 0x3F);
        }
        ok = ok && cp >= minimum && cp <= 0x10FFFF && !(cp >= 0xD800 && cp <= 0xDFFF);
        if (!ok) {
            if (policy == Policy::strict) return std::nullopt;
            out += replacement;
            ++i;
            continue;
        }
        append_utf16(out, cp);
        i += length;
    }
    return out;
}

std::optional<std::string> utf16_to_utf8(std::u16string_view in, Policy policy) {
    std::string out;
    out.reserve(in.size() * 3);
    for (std::size_t i = 0; i < in.size(); ++i) {
        char32_t cp = in[i];
        if (is_high(cp) && i + 1 < in.size() && is_low(in[i + 1])) {
            cp = 0x10000 + ((cp - 0xD800) << 10) + (in[i + 1] - 0xDC00);
            ++i;
        } else if (is_high(cp) || is_low(cp)) {
            if (policy == Policy::strict) return std::nullopt;
            cp = replacement;
        }
        append_utf8(out, cp);
    }
    return out;
}

std::optional<std::u16string> cp949_to_utf16(std::string_view in, Policy policy) {
    std::u16string out;
    out.reserve(in.size());
    for (std::size_t i = 0; i < in.size();) {
        const auto b = static_cast<unsigned char>(in[i]);
        if (b < 0x80) {
            out += static_cast<char16_t>(b);
            ++i;
            continue;
        }
        char16_t cp = 0;
        std::size_t used = 1;
        if (b >= lead_first && b <= lead_last && i + 1 < in.size()) {
            const auto t = static_cast<unsigned char>(in[i + 1]);
            if (t >= trail_first && t <= trail_last) {
                cp = forward_table[b - lead_first][t - trail_first];
                if (cp != 0) used = 2;
            }
        }
        if (cp == 0) {
            if (policy == Policy::strict) return std::nullopt;
            cp = replacement;
        }
        out += cp;
        i += used;
    }
    return out;
}

std::optional<std::string> utf16_to_cp949(std::u16string_view in, Policy policy) {
    std::string out;
    out.reserve(in.size() * 2);
    for (std::size_t i = 0; i < in.size(); ++i) {
        const char16_t cp = in[i];
        if (cp < 0x80) {
            out += static_cast<char>(cp);
            continue;
        }
        if (is_high(cp) && i + 1 < in.size() && is_low(in[i + 1])) ++i;  // non-BMP: one unmappable character
        else if (!is_high(cp) && !is_low(cp))
            if (const auto code = reverse().find(cp)) {
                out += static_cast<char>(*code >> 8);
                out += static_cast<char>(*code & 0xFF);
                continue;
            }
        if (policy == Policy::strict) return std::nullopt;
        out += '?';
    }
    return out;
}

char16_t cp949_decode(unsigned char lead, unsigned char trail) noexcept {
    if (lead < lead_first || lead > lead_last || trail < trail_first || trail > trail_last) return 0;
    return forward_table[lead - lead_first][trail - trail_first];
}
std::uint16_t cp949_encode(char16_t code_point) {
    const auto code = reverse().find(code_point);
    return code ? *code : 0;
}

std::optional<std::string> cp949_to_utf8(std::string_view in, Policy policy) {
    const auto wide = cp949_to_utf16(in, policy);
    return wide ? utf16_to_utf8(*wide, policy) : std::nullopt;
}
std::optional<std::string> utf8_to_cp949(std::string_view in, Policy policy) {
    const auto wide = utf8_to_utf16(in, policy);
    return wide ? utf16_to_cp949(*wide, policy) : std::nullopt;
}
}
