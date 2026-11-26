// SPDX-License-Identifier: GPL-2.0-or-later
#include "shell/json.hpp"
#include <cmath>
#include <cstdlib>

namespace atlan::shell {
namespace {
class Reader {
public:
    explicit Reader(std::string_view text) : text_(text) {}
    std::optional<JsonValue> document(std::string& why) {
        JsonValue value;
        if (!parse(value, 0) || (space(), pos_ != text_.size() && fail("trailing characters"))) {
            why = error_ + " at byte " + std::to_string(pos_);
            return std::nullopt;
        }
        return value;
    }
private:
    std::string_view text_;
    std::size_t pos_{};
    std::string error_;

    bool fail(const char* what) { if (error_.empty()) error_ = what; return false; }
    void space() { while (pos_ < text_.size() && (text_[pos_] == ' ' || text_[pos_] == '\t' || text_[pos_] == '\n' || text_[pos_] == '\r')) ++pos_; }
    bool literal(std::string_view word) {
        if (text_.substr(pos_, word.size()) != word) return fail("unknown literal");
        pos_ += word.size();
        return true;
    }
    bool hex4(unsigned& out) {
        if (pos_ + 4 > text_.size()) return fail("short \\u escape");
        out = 0;
        for (unsigned i = 0; i < 4; ++i) {
            const char c = text_[pos_++];
            out <<= 4;
            if (c >= '0' && c <= '9') out |= static_cast<unsigned>(c - '0');
            else if (c >= 'a' && c <= 'f') out |= static_cast<unsigned>(c - 'a' + 10);
            else if (c >= 'A' && c <= 'F') out |= static_cast<unsigned>(c - 'A' + 10);
            else return fail("bad \\u escape");
        }
        return true;
    }
    static void utf8(std::string& out, unsigned cp) {
        if (cp < 0x80) out += static_cast<char>(cp);
        else if (cp < 0x800) { out += static_cast<char>(0xc0 | cp >> 6); out += static_cast<char>(0x80 | (cp & 63)); }
        else if (cp < 0x10000) {
            out += static_cast<char>(0xe0 | cp >> 12); out += static_cast<char>(0x80 | (cp >> 6 & 63)); out += static_cast<char>(0x80 | (cp & 63));
        } else {
            out += static_cast<char>(0xf0 | cp >> 18); out += static_cast<char>(0x80 | (cp >> 12 & 63));
            out += static_cast<char>(0x80 | (cp >> 6 & 63)); out += static_cast<char>(0x80 | (cp & 63));
        }
    }
    bool string(std::string& out) {
        ++pos_;  // opening quote
        while (true) {
            if (pos_ >= text_.size()) return fail("unterminated string");
            const auto c = static_cast<unsigned char>(text_[pos_++]);
            if (c == '"') return true;
            if (c < 0x20) return fail("control character in string");
            if (c != '\\') { out += static_cast<char>(c); continue; }
            if (pos_ >= text_.size()) return fail("unterminated string");
            switch (text_[pos_++]) {
            case '"': out += '"'; break;
            case '\\': out += '\\'; break;
            case '/': out += '/'; break;
            case 'b': out += '\b'; break;
            case 'f': out += '\f'; break;
            case 'n': out += '\n'; break;
            case 'r': out += '\r'; break;
            case 't': out += '\t'; break;
            case 'u': {
                unsigned cp = 0;
                if (!hex4(cp)) return false;
                if (cp >= 0xdc00 && cp < 0xe000) return fail("lone low surrogate");
                if (cp >= 0xd800 && cp < 0xdc00) {
                    unsigned low = 0;
                    if (text_.substr(pos_, 2) != "\\u") return fail("lone high surrogate");
                    pos_ += 2;
                    if (!hex4(low)) return false;
                    if (low < 0xdc00 || low >= 0xe000) return fail("lone high surrogate");
                    cp = 0x10000 + ((cp - 0xd800) << 10) + (low - 0xdc00);
                }
                utf8(out, cp);
                break;
            }
            default: return fail("bad escape");
            }
        }
    }
    bool number(double& out) {
        const auto start = pos_;
        const auto digits = [&] { const auto from = pos_; while (pos_ < text_.size() && text_[pos_] >= '0' && text_[pos_] <= '9') ++pos_; return pos_ - from; };
        if (pos_ < text_.size() && text_[pos_] == '-') ++pos_;
        if (pos_ < text_.size() && text_[pos_] == '0') ++pos_;
        else if (digits() == 0) return fail("bad number");
        if (pos_ < text_.size() && text_[pos_] == '.') { ++pos_; if (digits() == 0) return fail("bad number"); }
        if (pos_ < text_.size() && (text_[pos_] == 'e' || text_[pos_] == 'E')) {
            ++pos_;
            if (pos_ < text_.size() && (text_[pos_] == '+' || text_[pos_] == '-')) ++pos_;
            if (digits() == 0) return fail("bad number");
        }
        const std::string copy(text_.substr(start, pos_ - start));
        out = std::strtod(copy.c_str(), nullptr);
        if (!std::isfinite(out)) return fail("number out of range");
        return true;
    }
    bool parse(JsonValue& value, unsigned depth) {
        if (depth > 32) return fail("nesting too deep");
        space();
        if (pos_ >= text_.size()) return fail("unexpected end");
        switch (text_[pos_]) {
        case 'n': value.kind = JsonValue::Kind::null; return literal("null");
        case 't': value.kind = JsonValue::Kind::boolean; value.boolean = true; return literal("true");
        case 'f': value.kind = JsonValue::Kind::boolean; value.boolean = false; return literal("false");
        case '"': value.kind = JsonValue::Kind::string; return string(value.string);
        case '[': {
            value.kind = JsonValue::Kind::array;
            ++pos_;
            space();
            if (pos_ < text_.size() && text_[pos_] == ']') { ++pos_; return true; }
            while (true) {
                value.array.emplace_back();
                if (!parse(value.array.back(), depth + 1)) return false;
                space();
                if (pos_ < text_.size() && text_[pos_] == ',') { ++pos_; continue; }
                if (pos_ < text_.size() && text_[pos_] == ']') { ++pos_; return true; }
                return fail("expected , or ]");
            }
        }
        case '{': {
            value.kind = JsonValue::Kind::object;
            ++pos_;
            space();
            if (pos_ < text_.size() && text_[pos_] == '}') { ++pos_; return true; }
            while (true) {
                space();
                if (pos_ >= text_.size() || text_[pos_] != '"') return fail("expected a key");
                std::string key;
                if (!string(key)) return false;
                space();
                if (pos_ >= text_.size() || text_[pos_] != ':') return fail("expected :");
                ++pos_;
                JsonValue member;
                if (!parse(member, depth + 1)) return false;
                if (!value.object.emplace(std::move(key), std::move(member)).second) return fail("duplicate key");
                space();
                if (pos_ < text_.size() && text_[pos_] == ',') { ++pos_; continue; }
                if (pos_ < text_.size() && text_[pos_] == '}') { ++pos_; return true; }
                return fail("expected , or }");
            }
        }
        default:
            value.kind = JsonValue::Kind::number;
            return number(value.number);
        }
    }
};
}

const JsonValue* JsonValue::find(std::string_view key) const {
    if (kind != Kind::object) return nullptr;
    const auto found = object.find(key);
    return found == object.end() ? nullptr : &found->second;
}

std::optional<JsonValue> parse_json(std::string_view text, std::string& why) { return Reader(text).document(why); }
}
