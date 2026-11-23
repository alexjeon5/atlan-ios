// SPDX-License-Identifier: GPL-2.0-or-later
#include "atlan/abi.hpp"
#include <array>
#include <bit>
#include <cstdio>

namespace atlan {
namespace {
constexpr std::size_t max_format = 4096, max_output = 4096, max_string = 4096;
bool read_word(Memory& memory, std::uint64_t at, std::uint32_t& out) noexcept {
    std::array<std::byte, 4> bytes{};
    if (at + 4 > 0x100000000ULL || !memory.read(static_cast<std::uint32_t>(at), bytes)) return false;
    out = 0;
    for (unsigned i = 0; i < 4; ++i) out |= std::to_integer<std::uint32_t>(bytes[i]) << (8 * i);
    return true;
}
}
VarArgs VarArgs::from_call(ImportCall& call, unsigned first_index) noexcept {
    VarArgs args;
    args.call_ = &call;
    args.slot_ = first_index;
    return args;
}
VarArgs VarArgs::from_va_list(Memory& memory, std::uint32_t list_pointer) noexcept {
    VarArgs args;
    args.memory_ = &memory;
    args.pointer_ = list_pointer;
    return args;
}
bool VarArgs::next32(std::uint32_t& out) noexcept {
    if (failed_) return false;
    if (call_) {
        failed_ = !call_->read_arg(slot_, out);
        if (!failed_) ++slot_;
    } else {
        failed_ = !read_word(*memory_, pointer_, out) || pointer_ > 0xfffffffbU;
        if (!failed_) pointer_ += 4;
    }
    return !failed_;
}
bool VarArgs::next64(std::uint64_t& out) noexcept {
    if (failed_) return false;
    // AAPCS32: doubleword arguments start at an even register or 8-byte aligned slot.
    if (call_) slot_ = (slot_ + 1) & ~1U;
    else pointer_ = (pointer_ + 7) & ~7U;
    std::uint32_t low{}, high{};
    if (!next32(low) || !next32(high)) return false;
    out = (std::uint64_t{high} << 32) | low;
    return true;
}
bool read_guest_string(Memory& memory, std::uint32_t address, std::size_t max_length, std::string& out) {
    out.clear();
    for (std::size_t i = 0; i <= max_length; ++i) {
        std::byte value{};
        if (std::uint64_t{address} + i > 0xffffffffULL ||
            !memory.read(static_cast<std::uint32_t>(address + i), std::span<std::byte>(&value, 1)))
            return false;
        if (value == std::byte{0}) return true;
        out.push_back(static_cast<char>(value));
    }
    return false;
}
std::optional<std::string> format_guest_printf(Memory& memory, std::uint32_t format, VarArgs& args,
                                               std::string& out) {
    std::string text;
    if (!read_guest_string(memory, format, max_format, text)) return "printf format string is unreadable or too long";
    const auto append = [&](std::string_view piece) -> bool {
        if (out.size() + piece.size() > max_output) return false;
        out.append(piece);
        return true;
    };
    const std::string overflow = "printf output exceeds service bound";
    for (std::size_t i = 0; i < text.size();) {
        if (text[i] != '%') {
            if (!append(std::string_view(&text[i], 1))) return overflow;
            ++i;
            continue;
        }
        ++i;
        std::string flags;
        while (i < text.size() && std::string_view("-+ #0").find(text[i]) != std::string_view::npos) flags += text[i++];
        const auto number = [&](bool& star, int& value) -> bool {
            star = false;
            value = -1;
            if (i < text.size() && text[i] == '*') {
                star = true;
                ++i;
                return true;
            }
            if (i < text.size() && text[i] >= '0' && text[i] <= '9') {
                long parsed = 0;
                while (i < text.size() && text[i] >= '0' && text[i] <= '9') {
                    parsed = parsed * 10 + (text[i++] - '0');
                    if (parsed > 4096) return false;
                }
                value = static_cast<int>(parsed);
            }
            return true;
        };
        bool star_width, star_precision = false;
        int width, precision = -1;
        if (!number(star_width, width)) return "printf width exceeds service bound";
        bool has_precision = false;
        if (i < text.size() && text[i] == '.') {
            ++i;
            has_precision = true;
            if (!number(star_precision, precision)) return "printf precision exceeds service bound";
            if (!star_precision && precision < 0) precision = 0;
        }
        if (star_width) {
            std::uint32_t value{};
            if (!args.next32(value)) return "printf '*' width argument is unreadable";
            width = static_cast<int>(value);
            if (width < 0) { flags += '-'; width = -width; }
            if (width > 4096) return "printf width exceeds service bound";
        }
        if (star_precision) {
            std::uint32_t value{};
            if (!args.next32(value)) return "printf '*' precision argument is unreadable";
            precision = static_cast<int>(value);
            if (precision < 0) has_precision = false;
            else if (precision > 4096) return "printf precision exceeds service bound";
        }
        int length = 0;  // 0 int, -1 short, -2 char, 1 long (32-bit), 2 long long
        if (i < text.size()) {
            if (text[i] == 'h') { ++i; length = -1; if (i < text.size() && text[i] == 'h') { ++i; length = -2; } }
            else if (text[i] == 'l') { ++i; length = 1; if (i < text.size() && text[i] == 'l') { ++i; length = 2; } }
            else if (text[i] == 'z' || text[i] == 't') { ++i; length = 1; }
            else if (text[i] == 'j') { ++i; length = 2; }
        }
        if (i >= text.size()) return "printf format ends inside a conversion";
        const char conversion = text[i++];
        std::string spec = "%" + flags;
        if (width >= 0) spec += std::to_string(width);
        if (has_precision) spec += "." + std::to_string(precision < 0 ? 0 : precision);
        std::array<char, 5200> buffer{};
        int produced = 0;
        switch (conversion) {
        case '%':
            if (!append("%")) return overflow;
            continue;
        case 'd': case 'i': case 'u': case 'x': case 'X': case 'o': case 'c': case 'p': {
            std::uint64_t raw{};
            if (length == 2) {
                if (!args.next64(raw)) return "printf 64-bit argument is unreadable";
            } else {
                std::uint32_t word{};
                if (!args.next32(word)) return "printf integer argument is unreadable";
                raw = word;
            }
            if (conversion == 'c') {
                spec += 'c';
                produced = std::snprintf(buffer.data(), buffer.size(), spec.c_str(), static_cast<int>(raw & 0xff));
            } else if (conversion == 'p') {
                spec = "%" + flags;
                if (width >= 0) spec += std::to_string(width);
                spec += "s";
                std::array<char, 16> digits{};
                std::snprintf(digits.data(), digits.size(), "0x%x", static_cast<unsigned>(raw));
                produced = std::snprintf(buffer.data(), buffer.size(), spec.c_str(), digits.data());
            } else if (conversion == 'd' || conversion == 'i') {
                std::int64_t value = length == 2 ? static_cast<std::int64_t>(raw)
                                   : length == -1 ? static_cast<std::int16_t>(raw)
                                   : length == -2 ? static_cast<std::int8_t>(raw)
                                                  : static_cast<std::int32_t>(raw);
                spec += "lld";
                produced = std::snprintf(buffer.data(), buffer.size(), spec.c_str(), static_cast<long long>(value));
            } else {
                std::uint64_t value = length == 2 ? raw
                                    : length == -1 ? static_cast<std::uint16_t>(raw)
                                    : length == -2 ? static_cast<std::uint8_t>(raw)
                                                   : static_cast<std::uint32_t>(raw);
                spec += "ll";
                spec += conversion;
                produced = std::snprintf(buffer.data(), buffer.size(), spec.c_str(), static_cast<unsigned long long>(value));
            }
            break;
        }
        case 'e': case 'E': case 'f': case 'F': case 'g': case 'G': {
            std::uint64_t raw{};
            if (!args.next64(raw)) return "printf double argument is unreadable";
            spec += conversion;
            produced = std::snprintf(buffer.data(), buffer.size(), spec.c_str(), std::bit_cast<double>(raw));
            break;
        }
        case 's': {
            std::uint32_t pointer{};
            if (!args.next32(pointer)) return "printf string argument is unreadable";
            std::string value;
            if (pointer == 0) value = "(null)";
            else {
                // With a precision only that many bytes may be read, as in C.
                const std::size_t bound = has_precision ? static_cast<std::size_t>(precision) : max_string;
                std::string full;
                if (has_precision) {
                    for (std::size_t n = 0; n < bound; ++n) {
                        std::byte b{};
                        if (std::uint64_t{pointer} + n > 0xffffffffULL ||
                            !memory.read(static_cast<std::uint32_t>(pointer + n), std::span<std::byte>(&b, 1)))
                            return "printf string argument reads unmapped guest memory";
                        if (b == std::byte{0}) break;
                        full.push_back(static_cast<char>(b));
                    }
                } else if (!read_guest_string(memory, pointer, bound, full))
                    return "printf string argument is unreadable or too long";
                value = std::move(full);
            }
            const std::size_t padded = static_cast<std::size_t>(width < 0 ? 0 : width);
            std::string piece = value;
            if (piece.size() < padded) {
                const std::string pad(padded - piece.size(), ' ');
                piece = flags.find('-') != std::string::npos ? piece + pad : pad + piece;
            }
            if (!append(piece)) return overflow;
            continue;
        }
        default:
            return std::string("printf conversion '%") + conversion + "' is not supported";
        }
        if (produced < 0 || static_cast<std::size_t>(produced) >= buffer.size()) return overflow;
        if (!append(std::string_view(buffer.data(), static_cast<std::size_t>(produced)))) return overflow;
    }
    return std::nullopt;
}
}
