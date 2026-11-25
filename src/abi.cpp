// SPDX-License-Identifier: GPL-2.0-or-later
#include "atlan/abi.hpp"
#include <array>
#include <bit>
#include <algorithm>
#include <cstdio>
#include <cstring>
#include <vector>

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
std::string guest_strerror(std::uint32_t code) {  // Linux/Bionic errno numbering
    switch (code) {
    case 0: return "Success";
    case 1: return "Operation not permitted";
    case 2: return "No such file or directory";
    case 5: return "I/O error";
    case 9: return "Bad file descriptor";
    case 12: return "Out of memory";
    case 13: return "Permission denied";
    case 17: return "File exists";
    case 20: return "Not a directory";
    case 21: return "Is a directory";
    case 22: return "Invalid argument";
    case 24: return "Too many open files";
    case 28: return "No space left on device";
    case 4: return "Interrupted system call";
    case 11: return "Try again";
    case 14: return "Bad address";
    case 32: return "Broken pipe";
    case 97: return "Address family not supported by protocol";
    case 98: return "Address already in use";
    case 99: return "Cannot assign requested address";
    case 100: return "Network is down";
    case 101: return "Network is unreachable";
    case 103: return "Software caused connection abort";
    case 104: return "Connection reset by peer";
    case 105: return "No buffer space available";
    case 106: return "Transport endpoint is already connected";
    case 107: return "Transport endpoint is not connected";
    case 110: return "Connection timed out";
    case 111: return "Connection refused";
    case 112: return "Host is down";
    case 113: return "No route to host";
    case 114: return "Operation already in progress";
    case 115: return "Operation now in progress";
    default: return "Unknown error " + std::to_string(code);
    }
}
void install_printf_services(ImportDispatcher& dispatcher) {
    struct Form { const char* name; bool bounded, list; };
    for (const Form form : {Form{"snprintf", true, false}, Form{"vsnprintf", true, true},
                            Form{"sprintf", false, false}, Form{"vsprintf", false, true}}) {
        dispatcher.bind(form.name, [form](ImportCall& call) -> std::optional<std::string> {
            const unsigned fixed = form.bounded ? 3 : 2;  // buf [, size], fmt
            const auto buffer = call.arg(0);
            const auto size = form.bounded ? call.arg(1) : 0xffffffffU;
            const auto format = call.arg(fixed - 1);
            auto args = form.list ? VarArgs::from_va_list(call.runtime().memory, call.arg(fixed))
                                  : VarArgs::from_call(call, fixed);
            std::string text;
            if (auto error = format_guest_printf(call.runtime().memory, format, args, text)) return std::string(form.name) + ": " + *error;
            if (!form.bounded || size != 0) {
                const std::size_t keep = form.bounded ? std::min<std::size_t>(text.size(), size - 1) : text.size();
                std::vector<std::byte> bytes(keep + 1, std::byte{0});
                for (std::size_t i = 0; i < keep; ++i) bytes[i] = static_cast<std::byte>(text[i]);
                if (!call.runtime().memory.write(buffer, bytes)) return std::string(form.name) + " guest memory fault";
            }
            call.set_result(static_cast<std::uint32_t>(text.size()));
            return std::nullopt;
        });
    }
}

void install_console_services(ImportDispatcher& dispatcher, std::uint32_t std_streams, std::uint32_t errno_address,
                              std::function<void(const std::string&)> sink) {
    // Bionic's FILE is 84 bytes; stdin, stdout and stderr are consecutive. Output to any other stream is not a console write.
    constexpr std::uint32_t file_size = 84;
    const auto console = std::make_shared<std::function<void(const std::string&)>>(std::move(sink));
    const auto write_to = [std_streams, console](ImportCall& call, std::uint32_t stream, const std::string& text, const char* name) -> std::optional<std::string> {
        if (stream != std_streams + file_size && stream != std_streams + 2 * file_size)
            return std::string(name) + " writes to a stream other than stdout/stderr, which is not implemented";
        if (*console) (*console)(text);
        (void)call;
        return std::nullopt;
    };
    const auto trim = [](std::string text) { while (!text.empty() && text.back() == '\n') text.pop_back(); return text; };
    // int printf(fmt, ...) / int vprintf(fmt, va)
    for (const bool list : {false, true}) {
        dispatcher.bind(list ? "vprintf" : "printf", [=](ImportCall& call) -> std::optional<std::string> {
            auto args = list ? VarArgs::from_va_list(call.runtime().memory, call.arg(1)) : VarArgs::from_call(call, 1);
            std::string text;
            if (auto error = format_guest_printf(call.runtime().memory, call.arg(0), args, text)) return "printf: " + *error;
            if (*console) (*console)(trim(text));
            call.set_result(static_cast<std::uint32_t>(text.size()));
            return std::nullopt;
        });
        // int fprintf(FILE*, fmt, ...) / int vfprintf(FILE*, fmt, va)
        dispatcher.bind(list ? "vfprintf" : "fprintf", [=](ImportCall& call) -> std::optional<std::string> {
            auto args = list ? VarArgs::from_va_list(call.runtime().memory, call.arg(2)) : VarArgs::from_call(call, 2);
            std::string text;
            if (auto error = format_guest_printf(call.runtime().memory, call.arg(1), args, text)) return "fprintf: " + *error;
            if (auto error = write_to(call, call.arg(0), trim(text), list ? "vfprintf" : "fprintf")) return error;
            call.set_result(static_cast<std::uint32_t>(text.size()));
            return std::nullopt;
        });
    }
    dispatcher.bind("puts", [=](ImportCall& call) -> std::optional<std::string> {
        std::string text;
        if (!read_guest_string(call.runtime().memory, call.arg(0), 1U << 20, text)) return "puts string is unreadable or too long";
        if (*console) (*console)(text);
        call.set_result(static_cast<std::uint32_t>(text.size() + 1));
        return std::nullopt;
    });
    dispatcher.bind("fputs", [=](ImportCall& call) -> std::optional<std::string> {
        std::string text;
        if (!read_guest_string(call.runtime().memory, call.arg(0), 1U << 20, text)) return "fputs string is unreadable or too long";
        if (auto error = write_to(call, call.arg(1), trim(text), "fputs")) return error;
        call.set_result(static_cast<std::uint32_t>(text.size()));
        return std::nullopt;
    });
    for (const char* name : {"fputc", "putc"}) {
        dispatcher.bind(name, [=](ImportCall& call) -> std::optional<std::string> {
            const auto c = call.arg(0) & 0xff;
            if (auto error = write_to(call, call.arg(1), c == '\n' ? std::string() : std::string(1, static_cast<char>(c)), name)) return error;
            call.set_result(c);
            return std::nullopt;
        });
    }
    // void perror(const char*): "<message>: <strerror(errno)>" on stderr.
    dispatcher.bind("perror", [=](ImportCall& call) -> std::optional<std::string> {
        std::string message;
        if (call.arg(0) != 0 && !read_guest_string(call.runtime().memory, call.arg(0), 1024, message)) return "perror message is unreadable";
        std::array<std::byte, 4> word{};
        std::uint32_t code = 0;
        if (errno_address != 0 && call.runtime().memory.read(errno_address, word))
            for (unsigned i = 0; i < 4; ++i) code |= std::to_integer<std::uint32_t>(word[i]) << (8 * i);
        if (*console) (*console)((message.empty() ? std::string() : message + ": ") + guest_strerror(code));
        return std::nullopt;
    });
    // char* strerror(int): the message in a read-write guest area mapped on first use, one 64-byte slot per code
    // (codes past the table share the last slot), so a returned pointer keeps its text as bionic's constants do.
    dispatcher.bind("strerror", [](ImportCall& call) -> std::optional<std::string> {
        constexpr std::uint32_t area = 0xf7010000, slot_bytes = 64, slots = 136;
        auto& memory = call.runtime().memory;
        if (memory.translate(area, MemoryAccess::write) == nullptr)
            memory.map(area, (slots * slot_bytes + Memory::page_size - 1) / Memory::page_size * Memory::page_size, Permissions::read_write);
        const std::uint32_t code = call.arg(0);
        std::string text = guest_strerror(code);
        if (text.size() > slot_bytes - 1) text.resize(slot_bytes - 1);
        std::vector<std::byte> bytes(text.size() + 1, std::byte{0});
        std::memcpy(bytes.data(), text.data(), text.size());
        const std::uint32_t at = area + std::min<std::uint32_t>(code, slots - 1) * slot_bytes;
        if (!memory.write(at, bytes)) return "strerror could not write its message";
        call.set_result(at);
        return std::nullopt;
    });
}
}
