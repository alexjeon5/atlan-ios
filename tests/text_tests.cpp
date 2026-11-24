// SPDX-License-Identifier: GPL-2.0-or-later
#include "atlan/text_codec.hpp"
#include <iostream>
#include <string>

namespace {
int checks = 0;
void check(bool ok, const char* what) {
    ++checks;
    if (!ok) throw std::runtime_error(what);
}
using atlan::text::Policy;
std::string bytes(std::initializer_list<unsigned> list) {
    std::string s;
    for (auto b : list) s += static_cast<char>(b);
    return s;
}
}
int main() {
    try {
        using namespace atlan::text;
        // "가" = B0A1, "힝" = C8FE, "한글" = C7D1 B1DB, "A" ASCII mixed in.
        check(cp949_to_utf16(bytes({0xB0, 0xA1})) == u"가", "ga");
        check(cp949_to_utf16(bytes({0xC8, 0xFE})) == u"힝", "last syllable of KS X 1001");
        check(cp949_to_utf8(bytes({'A', 0xC7, 0xD1, 0xB1, 0xDB, 'Z'})) == "A한글Z", "mixed ASCII and Hangul to UTF-8");
        check(utf8_to_cp949("A한글Z") == bytes({'A', 0xC7, 0xD1, 0xB1, 0xDB, 'Z'}), "UTF-8 to CP949");
        // UHC extension syllables outside KS X 1001, and Hanja.
        check(utf16_to_cp949(u"똠") == bytes({0x8C, 0x63}) && utf16_to_cp949(u"힣") == bytes({0xC6, 0x52}), "extension syllables");
        check(utf16_to_cp949(u"漢") == bytes({0xF9, 0xD3}), "Hanja");
        // Round trip every mapped double-byte code.
        std::size_t mapped = 0;
        for (unsigned lead = 0x81; lead <= 0xFE; ++lead)
            for (unsigned trail = 0x41; trail <= 0xFE; ++trail) {
                const auto in = bytes({lead, trail});
                if (const auto wide = cp949_to_utf16(in)) {
                    ++mapped;
                    check(wide->size() == 1 && utf16_to_cp949(*wide) == in, "round trip of a mapped code");
                }
            }
        check(mapped == 17048, "all 17048 double-byte codes map");
        // Strict failures and replacements.
        check(!cp949_to_utf16(bytes({0x80})) && !cp949_to_utf16(bytes({0xFF})), "invalid lone bytes");
        check(!cp949_to_utf16(bytes({0xB0})), "truncated pair");
        check(!cp949_to_utf16(bytes({0xB0, 0x20})), "bad trail byte");
        check(cp949_to_utf16(bytes({0xB0, 0x20, 'x'}), Policy::replace) == std::u16string{0xFFFD, u' ', u'x'}, "replace keeps following text");
        check(!utf16_to_cp949(u"\U0001F600") && utf16_to_cp949(u"a\U0001F600b", Policy::replace) == "a?b", "non-BMP is unmappable, one replacement");
        check(utf16_to_cp949(u"é", Policy::replace) == "?", "unmappable BMP char");
        // UTF-8 / UTF-16.
        check(utf8_to_utf16("héllo 한글 \xF0\x9F\x98\x80") == std::u16string(u"héllo 한글 \U0001F600"), "UTF-8 to UTF-16 with a surrogate pair");
        check(utf16_to_utf8(u"héllo 한글 \U0001F600") == "héllo 한글 \xF0\x9F\x98\x80", "UTF-16 to UTF-8");
        check(!utf8_to_utf16("\xC0\x80") && !utf8_to_utf16("\xED\xA0\x80") && !utf8_to_utf16("\xE0\x80") && !utf8_to_utf16("\xF5\x80\x80\x80"), "overlong, surrogate, truncated, too large");
        check(utf8_to_utf16("a\xFFz", Policy::replace) == std::u16string{u'a', 0xFFFD, u'z'}, "UTF-8 replacement");
        check(!utf16_to_utf8(std::u16string{0xD800}) && utf16_to_utf8(std::u16string{0xDC00, u'a'}, Policy::replace) == "\xEF\xBF\xBD" "a", "lone surrogates");
        check(cp949_to_utf8("")->empty() && utf8_to_cp949("")->empty(), "empty strings");
    } catch (const std::exception& error) {
        std::cerr << "text codec test failed: " << error.what() << '\n';
        return 1;
    }
    std::cout << "text codec checks passed: " << checks << '\n';
}
