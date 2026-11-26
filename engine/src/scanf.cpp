// SPDX-License-Identifier: GPL-2.0-or-later
#include "atlan/scanf.hpp"
#include "atlan/abi.hpp"
#include <array>
#include <cctype>
#include <cerrno>
#include <cstdlib>
#include <cstring>

namespace atlan {
namespace {
bool store(Memory& memory, std::uint32_t at, std::uint64_t value, unsigned size) {
    std::array<std::byte, 8> bytes{};
    for (unsigned i = 0; i < size; ++i) bytes[i] = std::byte((value >> (8 * i)) & 255U);
    return memory.write(at, std::span<const std::byte>(bytes.data(), size));
}
}

std::optional<std::string> scan_guest(Memory& memory, const std::string& in, const std::string& format, VarArgs& args, int& assigned) {
    assigned = 0;
    std::size_t at = 0;
    bool converted = false;  // any conversion attempted with input present
    const auto skip_space = [&] { while (at < in.size() && std::isspace(static_cast<unsigned char>(in[at]))) ++at; };
    const auto finish = [&](bool input_failure) {
        if (input_failure && !converted && assigned == 0) assigned = -1;
        return std::nullopt;
    };
    for (std::size_t f = 0; f < format.size(); ++f) {
        const unsigned char c = static_cast<unsigned char>(format[f]);
        if (std::isspace(c)) { skip_space(); continue; }
        if (c != '%') {
            if (at >= in.size()) return finish(true);
            if (in[at] != static_cast<char>(c)) return finish(false);
            ++at;
            continue;
        }
        if (++f >= format.size()) return "sscanf format ends inside a conversion";
        if (format[f] == '%') {
            skip_space();
            if (at >= in.size()) return finish(true);
            if (in[at] != '%') return finish(false);
            ++at;
            continue;
        }
        bool suppress = false;
        if (format[f] == '*') { suppress = true; ++f; }
        std::size_t width = 0;
        while (f < format.size() && std::isdigit(static_cast<unsigned char>(format[f]))) width = width * 10 + static_cast<std::size_t>(format[f++] - '0');
        unsigned size = 4;
        bool wide_float = false;
        if (f < format.size()) {
            switch (format[f]) {
            case 'h': if (f + 1 < format.size() && format[f + 1] == 'h') { size = 1; f += 2; } else { size = 2; ++f; } break;
            case 'l': if (f + 1 < format.size() && format[f + 1] == 'l') { size = 8; f += 2; } else { wide_float = true; ++f; } break;
            case 'L': case 'q': size = 8; wide_float = true; ++f; break;
            case 'j': size = 8; ++f; break;
            case 'z': case 't': ++f; break;
            default: break;
            }
        }
        if (f >= format.size()) return "sscanf format ends inside a conversion";
        const char conversion = format[f];
        std::uint32_t pointer = 0;
        const auto target = [&]() -> bool { return suppress || args.next32(pointer); };
        switch (conversion) {
        case 'n':
            if (!target()) return "sscanf argument is unreadable";
            if (!suppress && !store(memory, pointer, at, size)) return "sscanf %n pointer is not writable";
            continue;
        case 'd': case 'i': case 'u': case 'o': case 'x': case 'X': case 'p': {
            skip_space();
            if (at >= in.size()) return finish(true);
            converted = true;
            std::string field = in.substr(at, width != 0 ? width : std::string::npos);
            const int base = conversion == 'd' || conversion == 'u' ? 10 : conversion == 'i' ? 0 : conversion == 'o' ? 8 : 16;
            char* end = nullptr;
            errno = 0;
            std::uint64_t value = 0;
            if (conversion == 'd' || conversion == 'i') value = static_cast<std::uint64_t>(std::strtoll(field.c_str(), &end, base));
            else value = std::strtoull(field.c_str(), &end, base);
            if (end == field.c_str()) return finish(false);
            at += static_cast<std::size_t>(end - field.c_str());
            if (!target()) return "sscanf argument is unreadable";
            if (!suppress) {
                if (!store(memory, pointer, value, conversion == 'p' ? 4 : size)) return "sscanf integer pointer is not writable";
                ++assigned;
            }
            continue;
        }
        case 'f': case 'e': case 'g': case 'E': case 'G': case 'a': case 'F': case 'A': {
            skip_space();
            if (at >= in.size()) return finish(true);
            converted = true;
            std::string field = in.substr(at, width != 0 ? width : std::string::npos);
            char* end = nullptr;
            const double value = std::strtod(field.c_str(), &end);
            if (end == field.c_str()) return finish(false);
            at += static_cast<std::size_t>(end - field.c_str());
            if (!target()) return "sscanf argument is unreadable";
            if (!suppress) {
                std::uint64_t bits = 0;
                unsigned bytes = 4;
                if (wide_float) { std::memcpy(&bits, &value, 8); bytes = 8; }
                else { const float narrow = static_cast<float>(value); std::uint32_t b32 = 0; std::memcpy(&b32, &narrow, 4); bits = b32; }
                if (!store(memory, pointer, bits, bytes)) return "sscanf float pointer is not writable";
                ++assigned;
            }
            continue;
        }
        case 's': case 'c': case '[': {
            std::string set;
            bool negate = false;
            if (conversion == '[') {
                ++f;
                if (f < format.size() && format[f] == '^') { negate = true; ++f; }
                if (f < format.size() && format[f] == ']') { set += ']'; ++f; }
                while (f < format.size() && format[f] != ']') {
                    if (format[f] == '-' && !set.empty() && f + 1 < format.size() && format[f + 1] != ']') {
                        for (int ch = static_cast<unsigned char>(set.back()) + 1; ch <= static_cast<unsigned char>(format[f + 1]); ++ch) set += static_cast<char>(ch);
                        f += 2;
                    } else {
                        set += format[f++];
                    }
                }
                if (f >= format.size()) return "sscanf scan set is not closed";
            }
            if (conversion == 's') skip_space();
            if (at >= in.size()) return finish(true);
            converted = true;
            const std::size_t limit = width != 0 ? width : conversion == 'c' ? 1 : std::string::npos;
            std::size_t n = 0;
            while (at + n < in.size() && n < limit) {
                const unsigned char ch = static_cast<unsigned char>(in[at + n]);
                if (conversion == 's' && std::isspace(ch)) break;
                if (conversion == '[' && (set.find(static_cast<char>(ch)) != std::string::npos) == negate) break;
                ++n;
            }
            if (n == 0) return finish(false);
            if (conversion == 'c' && n < limit) return finish(true);
            if (!target()) return "sscanf argument is unreadable";
            if (!suppress) {
                std::string text = in.substr(at, n);
                if (conversion != 'c') text += '\0';
                if (!memory.write(pointer, std::span<const std::byte>(reinterpret_cast<const std::byte*>(text.data()), text.size())))
                    return "sscanf string pointer is not writable";
                ++assigned;
            }
            at += n;
            continue;
        }
        default:
            return std::string("sscanf conversion %") + conversion + " is not supported";
        }
    }
    return std::nullopt;
}

void install_scanf_services(ImportDispatcher& dispatcher) {
    // int sscanf(const char* input, const char* format, ...)
    dispatcher.bind("sscanf", [](ImportCall& call) -> std::optional<std::string> {
        std::string input, format;
        if (call.arg(0) == 0 || !read_guest_string(call.runtime().memory, call.arg(0), 1U << 20, input)) return "sscanf input is unreadable";
        if (call.arg(1) == 0 || !read_guest_string(call.runtime().memory, call.arg(1), 4096, format)) return "sscanf format is unreadable";
        auto args = VarArgs::from_call(call, 2);
        int assigned = 0;
        if (auto error = scan_guest(call.runtime().memory, input, format, args, assigned)) return error;
        call.set_result(static_cast<std::uint32_t>(assigned));
        return std::nullopt;
    });
}
}
