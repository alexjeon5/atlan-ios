// SPDX-License-Identifier: GPL-2.0-or-later
#include "atlan/libc_services.hpp"
#include <array>
#include <chrono>
#include <memory>
#include <vector>

namespace atlan {
namespace {
constexpr std::uint32_t max_block = 64U * 1024 * 1024;  // bound one service call
}
void install_libc_services(ImportDispatcher& dispatcher) {
    // POSIX drand48 family (only srand48/lrand48 are imported): X = (0x5DEECE66D * X + 0xB) mod 2^48,
    // seeded as (seed << 16) | 0x330E; the default state is 0x1234ABCD330E.
    const auto rand48 = std::make_shared<std::uint64_t>(0x1234ABCD330EULL);
    dispatcher.bind("srand48", [rand48](ImportCall& call) -> std::optional<std::string> {
        *rand48 = (std::uint64_t{call.arg(0)} << 16) | 0x330E;
        return std::nullopt;
    });
    dispatcher.bind("lrand48", [rand48](ImportCall& call) -> std::optional<std::string> {
        *rand48 = (0x5DEECE66DULL * *rand48 + 0xB) & ((1ULL << 48) - 1);
        call.set_result(static_cast<std::uint32_t>(*rand48 >> 17));
        return std::nullopt;
    });
    // void* memset(void* dest, int value, size_t count)
    dispatcher.bind("memset", [](ImportCall& call) -> std::optional<std::string> {
        const auto dest = call.arg(0), count = call.arg(2);
        if (count > max_block) return "memset size exceeds service bound";
        if (count != 0 && std::uint64_t{dest} + count > 0x100000000ULL) return "memset wraps the guest address space";
        const std::vector<std::byte> block(count, std::byte(call.arg(1) & 0xff));
        if (!call.runtime().memory.write(dest, block)) return "memset guest memory fault";
        call.set_result(dest);
        return std::nullopt;
    });
    // size_t strlen(const char* text): bounded scan of guest memory.
    dispatcher.bind("strlen", [](ImportCall& call) -> std::optional<std::string> {
        const auto start = call.arg(0);
        for (std::uint32_t length = 0; length < max_block; ++length) {
            std::byte value{};
            if (std::uint64_t{start} + length > 0xffffffffULL ||
                !call.runtime().memory.read(start + length, std::span<std::byte>(&value, 1)))
                return "strlen read left mapped guest memory";
            if (value == std::byte{0}) { call.set_result(length); return std::nullopt; }
        }
        return "strlen exceeds service bound";
    });
    // char* strcpy(char* dest, const char* src): copies through the terminator.
    dispatcher.bind("strcpy", [](ImportCall& call) -> std::optional<std::string> {
        const auto dest = call.arg(0), src = call.arg(1);
        auto& memory = call.runtime().memory;
        std::vector<std::byte> text;
        for (std::uint32_t i = 0; i < max_block; ++i) {
            std::byte value{};
            if (std::uint64_t{src} + i > 0xffffffffULL || !memory.read(src + i, std::span<std::byte>(&value, 1)))
                return "strcpy read left mapped guest memory";
            text.push_back(value);
            if (value == std::byte{0}) {
                if (std::uint64_t{dest} + text.size() > 0x100000000ULL || !memory.write(dest, text))
                    return "strcpy guest memory fault";
                call.set_result(dest);
                return std::nullopt;
            }
        }
        return "strcpy exceeds service bound";
    });
    // void* memcpy(void* dest, const void* src, size_t count): staged through a host
    // buffer, so overlapping ranges behave like memmove instead of corrupting data.
    for (const char* name : {"memcpy", "memmove"})
        dispatcher.bind(name, [name](ImportCall& call) -> std::optional<std::string> {
            const auto dest = call.arg(0), src = call.arg(1), count = call.arg(2);
            if (count > max_block) return std::string(name) + " size exceeds service bound";
            auto& memory = call.runtime().memory;
            std::vector<std::byte> data(count);
            if ((count != 0 && (std::uint64_t{src} + count > 0x100000000ULL || std::uint64_t{dest} + count > 0x100000000ULL)) ||
                !memory.read(src, data) || !memory.write(dest, data))
                return std::string(name) + " guest memory fault";
            call.set_result(dest);
            return std::nullopt;
        });
    // C-locale <ctype.h>: ASCII only, EOF and values above 255 are not characters.
    const auto ctype = [&dispatcher](const char* name, bool (*test)(std::uint32_t)) {
        dispatcher.bind(name, [test](ImportCall& call) -> std::optional<std::string> {
            const auto c = call.arg(0);
            call.set_result(c <= 255 && test(c) ? 1 : 0);
            return std::nullopt;
        });
    };
    ctype("isalpha", [](std::uint32_t c) { return (c | 32) >= 'a' && (c | 32) <= 'z'; });
    ctype("isdigit", [](std::uint32_t c) { return c >= '0' && c <= '9'; });
    ctype("isalnum", [](std::uint32_t c) { return ((c | 32) >= 'a' && (c | 32) <= 'z') || (c >= '0' && c <= '9'); });
    ctype("islower", [](std::uint32_t c) { return c >= 'a' && c <= 'z'; });
    ctype("isupper", [](std::uint32_t c) { return c >= 'A' && c <= 'Z'; });
    ctype("isspace", [](std::uint32_t c) { return c == ' ' || (c >= 9 && c <= 13); });
    ctype("iscntrl", [](std::uint32_t c) { return c < 32 || c == 127; });
    ctype("isprint", [](std::uint32_t c) { return c >= 32 && c < 127; });
    ctype("isgraph", [](std::uint32_t c) { return c > 32 && c < 127; });
    ctype("ispunct", [](std::uint32_t c) { return c > 32 && c < 127 && !((c | 32) >= 'a' && (c | 32) <= 'z') && !(c >= '0' && c <= '9'); });
    ctype("isxdigit", [](std::uint32_t c) { return (c >= '0' && c <= '9') || ((c | 32) >= 'a' && (c | 32) <= 'f'); });
    // size_t wcslen(const wchar_t*): wchar_t is 32 bits on Android ARM.
    dispatcher.bind("wcslen", [](ImportCall& call) -> std::optional<std::string> {
        const auto start = call.arg(0);
        if (start % 4) return "wcslen on a misaligned pointer";
        for (std::uint32_t length = 0; length < max_block / 4; ++length) {
            std::array<std::byte, 4> word{};
            if (std::uint64_t{start} + 4ULL * length + 4 > 0x100000000ULL ||
                !call.runtime().memory.read(start + 4 * length, word))
                return "wcslen read left mapped guest memory";
            if (word == std::array<std::byte, 4>{}) { call.set_result(length); return std::nullopt; }
        }
        return "wcslen exceeds service bound";
    });
    // wchar_t* wmemmove(wchar_t* dest, const wchar_t* src, size_t count): count is in wide characters.
    dispatcher.bind("wmemmove", [](ImportCall& call) -> std::optional<std::string> {
        const auto dest = call.arg(0), src = call.arg(1);
        const std::uint64_t bytes = std::uint64_t{call.arg(2)} * 4;
        if (bytes > max_block) return "wmemmove size exceeds service bound";
        auto& memory = call.runtime().memory;
        std::vector<std::byte> data(bytes);
        if ((bytes != 0 && (std::uint64_t{src} + bytes > 0x100000000ULL || std::uint64_t{dest} + bytes > 0x100000000ULL)) ||
            !memory.read(src, data) || !memory.write(dest, data))
            return "wmemmove guest memory fault";
        call.set_result(dest);
        return std::nullopt;
    });
    // long sysconf(int name): only _SC_PAGESIZE (39 in Bionic ARM) is modelled; other names stop explicitly.
    dispatcher.bind("sysconf", [](ImportCall& call) -> std::optional<std::string> {
        if (call.arg(0) != 39) return "sysconf name " + std::to_string(call.arg(0)) + " is not modelled";
        call.set_result(Memory::page_size);
        return std::nullopt;
    });
    // char* strcat(char* dest, const char* src): appends through the terminator.
    dispatcher.bind("strcat", [](ImportCall& call) -> std::optional<std::string> {
        const auto dest = call.arg(0), src = call.arg(1);
        auto& memory = call.runtime().memory;
        const auto scan = [&](std::uint32_t at, std::vector<std::byte>* out) -> std::optional<std::uint32_t> {
            for (std::uint32_t i = 0; i < max_block; ++i) {
                std::byte value{};
                if (std::uint64_t{at} + i > 0xffffffffULL || !memory.read(at + i, std::span<std::byte>(&value, 1)))
                    return std::nullopt;
                if (out) out->push_back(value);
                if (value == std::byte{0}) return i;
            }
            return std::nullopt;
        };
        const auto end = scan(dest, nullptr);
        std::vector<std::byte> text;
        if (!end || !scan(src, &text)) return "strcat read left mapped or bounded guest memory";
        if (std::uint64_t{dest} + *end + text.size() > 0x100000000ULL || !memory.write(dest + *end, text))
            return "strcat guest memory fault";
        call.set_result(dest);
        return std::nullopt;
    });
    // void __stack_chk_fail(void): the guest's stack canary was overwritten.
    dispatcher.bind("__stack_chk_fail", [](ImportCall&) -> std::optional<std::string> {
        return "guest stack protector failure (__stack_chk_fail)";
    });
    // int clock_gettime(clockid_t clock, struct timespec* out), 32-bit Linux ABI.
    // Only CLOCK_REALTIME (0) and CLOCK_MONOTONIC (1) are supported.
    dispatcher.bind("clock_gettime", [](ImportCall& call) -> std::optional<std::string> {
        const auto clock = call.arg(0);
        std::chrono::nanoseconds now;
        if (clock == 0) now = std::chrono::system_clock::now().time_since_epoch();
        else if (clock == 1) now = std::chrono::steady_clock::now().time_since_epoch();
        else return "clock_gettime clock id " + std::to_string(clock) + " is not supported";
        const auto seconds = std::chrono::duration_cast<std::chrono::seconds>(now);
        const auto nanos = static_cast<std::uint32_t>((now - seconds).count());
        const std::uint32_t words[2]{static_cast<std::uint32_t>(seconds.count()), nanos};
        std::array<std::byte, 8> bytes{};
        for (unsigned i = 0; i < 8; ++i) bytes[i] = std::byte((words[i / 4] >> (8 * (i % 4))) & 255);
        if (!call.runtime().memory.write(call.arg(1), bytes)) return "clock_gettime guest memory fault";
        call.set_result(0);
        return std::nullopt;
    });
    // int gettimeofday(struct timeval* tv, struct timezone* tz): wall clock, 32-bit ABI; tz is reported as UTC.
    dispatcher.bind("gettimeofday", [](ImportCall& call) -> std::optional<std::string> {
        const auto now = std::chrono::system_clock::now().time_since_epoch();
        const auto seconds = std::chrono::duration_cast<std::chrono::seconds>(now);
        const auto micros = static_cast<std::uint32_t>(std::chrono::duration_cast<std::chrono::microseconds>(now - seconds).count());
        const std::uint32_t words[2]{static_cast<std::uint32_t>(seconds.count()), micros};
        if (call.arg(0) != 0) {
            std::array<std::byte, 8> bytes{};
            for (unsigned i = 0; i < 8; ++i) bytes[i] = std::byte((words[i / 4] >> (8 * (i % 4))) & 255);
            if (!call.runtime().memory.write(call.arg(0), bytes)) return "gettimeofday guest memory fault";
        }
        if (call.arg(1) != 0) {
            const std::array<std::byte, 8> zone{};
            if (!call.runtime().memory.write(call.arg(1), zone)) return "gettimeofday timezone guest memory fault";
        }
        call.set_result(0);
        return std::nullopt;
    });
    // time_t time(time_t* out)
    dispatcher.bind("time", [](ImportCall& call) -> std::optional<std::string> {
        const auto seconds = static_cast<std::uint32_t>(std::chrono::duration_cast<std::chrono::seconds>(
            std::chrono::system_clock::now().time_since_epoch()).count());
        if (call.arg(0) != 0) {
            std::array<std::byte, 4> bytes{};
            for (unsigned i = 0; i < 4; ++i) bytes[i] = std::byte((seconds >> (8 * i)) & 255);
            if (!call.runtime().memory.write(call.arg(0), bytes)) return "time guest memory fault";
        }
        call.set_result(seconds);
        return std::nullopt;
    });
}
}
