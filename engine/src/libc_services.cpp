// SPDX-License-Identifier: GPL-2.0-or-later
#include "atlan/libc_services.hpp"
#include <array>
#include <chrono>
#include <algorithm>
#include <cstring>
#include <ctime>
#include <map>
#include <memory>
#include <vector>

namespace atlan {
namespace {
constexpr std::uint32_t max_block = 64U * 1024 * 1024;  // bound one service call
// Fast paths for the block cache (ImportDispatcher::bind_inline). Each does what the service below does, and leaves
// every call the service would fail to it, before changing anything.
[[nodiscard]] std::uint32_t reg(const CpuState& cpu, unsigned index) noexcept { return static_cast<std::uint32_t>(cpu.registers[index]); }
bool memset_inline(CpuState& cpu, Memory& memory) noexcept {
    const auto dest = reg(cpu, 0), count = reg(cpu, 2);
    if (count > max_block || (count != 0 && std::uint64_t{dest} + count > 0x100000000ULL)) return false;
    if (!memory.fill(dest, std::byte(reg(cpu, 1) & 0xff), count)) return false;
    return true;  // r0 is already dest
}
bool memmove_inline(CpuState& cpu, Memory& memory) noexcept {
    const auto dest = reg(cpu, 0), src = reg(cpu, 1), count = reg(cpu, 2);
    if (count > max_block) return false;
    if (count != 0 && (std::uint64_t{src} + count > 0x100000000ULL || std::uint64_t{dest} + count > 0x100000000ULL)) return false;
    return memory.move(dest, src, count);
}
bool strlen_inline(CpuState& cpu, Memory& memory) noexcept {
    const auto start = reg(cpu, 0);
    for (std::uint64_t length = 0; length < max_block;) {
        if (std::uint64_t{start} + length > 0xffffffffULL) return false;
        const auto bytes = memory.page_span(static_cast<std::uint32_t>(start + length), MemoryAccess::read);
        if (bytes.empty()) return false;
        const auto scan = static_cast<std::size_t>(std::min<std::uint64_t>(bytes.size(), max_block - length));
        if (const void* end = std::memchr(bytes.data(), 0, scan)) {
            cpu.registers[0] = static_cast<std::uint32_t>(length + static_cast<std::size_t>(static_cast<const std::byte*>(end) - bytes.data()));
            return true;
        }
        length += scan;
    }
    return false;
}
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
        if (!call.runtime().memory.fill(dest, std::byte(call.arg(1) & 0xff), count)) return "memset guest memory fault";
        call.set_result(dest);
        return std::nullopt;
    });
    // size_t strlen(const char* text): bounded scan of guest memory.
    dispatcher.bind("strlen", [](ImportCall& call) -> std::optional<std::string> {
        const auto start = call.arg(0);
        for (std::uint64_t length = 0; length < max_block;) {
            if (std::uint64_t{start} + length > 0xffffffffULL) return "strlen read left mapped guest memory";
            const auto bytes = call.runtime().memory.page_span(static_cast<std::uint32_t>(start + length), MemoryAccess::read);
            if (bytes.empty()) return "strlen read left mapped guest memory";
            const auto scan = static_cast<std::size_t>(std::min<std::uint64_t>(bytes.size(), max_block - length));
            if (const void* end = std::memchr(bytes.data(), 0, scan)) {
                call.set_result(static_cast<std::uint32_t>(length + static_cast<std::size_t>(static_cast<const std::byte*>(end) - bytes.data())));
                return std::nullopt;
            }
            length += scan;
        }
        return "strlen exceeds service bound";
    });
    dispatcher.bind_inline("memset", memset_inline);
    dispatcher.bind_inline("strlen", strlen_inline);
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
            if ((count != 0 && (std::uint64_t{src} + count > 0x100000000ULL || std::uint64_t{dest} + count > 0x100000000ULL)) ||
                !memory.move(dest, src, count))
                return std::string(name) + " guest memory fault";
            call.set_result(dest);
            return std::nullopt;
        });
    dispatcher.bind_inline("memcpy", memmove_inline);
    dispatcher.bind_inline("memmove", memmove_inline);
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
    // setpriority/getpriority(PRIO_PROCESS, id): guest threads are cooperative and the host schedules them, so the
    // value is kept as state only (what getpriority reports back); it does not change scheduling.
    const auto nice = std::make_shared<std::map<std::uint32_t, std::int32_t>>();
    dispatcher.bind("setpriority", [nice](ImportCall& call) -> std::optional<std::string> {
        if (call.arg(0) != 0) return "setpriority supports only PRIO_PROCESS";
        const auto value = static_cast<std::int32_t>(call.arg(2));
        (*nice)[call.arg(1)] = value < -20 ? -20 : value > 19 ? 19 : value;
        call.set_result(0);
        return std::nullopt;
    });
    dispatcher.bind("getpriority", [nice](ImportCall& call) -> std::optional<std::string> {
        if (call.arg(0) != 0) return "getpriority supports only PRIO_PROCESS";
        const auto found = nice->find(call.arg(1));
        call.set_result(static_cast<std::uint32_t>(found == nice->end() ? 0 : found->second));
        return std::nullopt;
    });
    // setjmp/longjmp family. The jmp_buf holds, in this host's own layout: r4-r11, sp, lr (bit 0 = Thumb state),
    // then d8-d15 (26 words, within Bionic's 64-word jmp_buf). A jump returns to the saved lr with the saved
    // callee-saved state, so it works across any number of frames and Thumb/ARM callers. Signal masks are not kept.
    for (const char* name : {"setjmp", "_setjmp", "sigsetjmp"}) {
        dispatcher.bind(name, [name](ImportCall& call) -> std::optional<std::string> {
            auto& cpu = call.runtime().cpu;
            std::array<std::byte, 26 * 4> raw{};
            const auto put = [&](unsigned word, std::uint32_t value) {
                for (unsigned b = 0; b < 4; ++b) raw[4 * word + b] = std::byte((value >> (8 * b)) & 255);
            };
            for (unsigned i = 0; i < 8; ++i) put(i, static_cast<std::uint32_t>(cpu.registers[4 + i]));
            put(8, static_cast<std::uint32_t>(cpu.registers[13]));
            put(9, static_cast<std::uint32_t>(cpu.registers[14]));
            std::memcpy(raw.data() + 40, reinterpret_cast<const std::byte*>(cpu.vectors.data()) + 64, 64);
            if (!call.runtime().memory.write(call.arg(0), raw)) return std::string(name) + " jmp_buf is not writable";
            call.set_result(0);
            return std::nullopt;
        });
    }
    for (const char* name : {"longjmp", "_longjmp", "siglongjmp"}) {
        dispatcher.bind(name, [name](ImportCall& call) -> std::optional<std::string> {
            auto& cpu = call.runtime().cpu;
            std::array<std::byte, 26 * 4> raw{};
            if (!call.runtime().memory.read(call.arg(0), raw)) return std::string(name) + " jmp_buf is unreadable";
            const auto get = [&](unsigned word) {
                std::uint32_t value = 0;
                for (unsigned b = 0; b < 4; ++b) value |= std::uint32_t(std::to_integer<std::uint8_t>(raw[4 * word + b])) << (8 * b);
                return value;
            };
            if (get(8) == 0 || get(9) == 0) return std::string(name) + " jmp_buf was never filled by setjmp";
            const std::uint32_t value = call.arg(1) == 0 ? 1 : call.arg(1);
            for (unsigned i = 0; i < 8; ++i) cpu.registers[4 + i] = get(i);
            cpu.registers[13] = get(8);
            cpu.registers[14] = get(9);
            std::memcpy(reinterpret_cast<std::byte*>(cpu.vectors.data()) + 64, raw.data() + 40, 64);
            call.set_result(value);
            call.runtime().start(get(9));  // resume at the setjmp return address instead of longjmp's caller
            call.transfer();
            return std::nullopt;
        });
    }
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

namespace atlan {
namespace {
// Bionic 32-bit struct tm: nine ints, then tm_gmtoff (long) and tm_zone (const char*).
bool read_tm(Memory& memory, std::uint32_t address, std::tm& out) {
    std::array<std::byte, 36> raw{};
    if (!memory.read(address, raw)) return false;
    std::int32_t v[9];
    for (unsigned i = 0; i < 9; ++i) {
        std::uint32_t word = 0;
        for (unsigned b = 0; b < 4; ++b) word |= std::uint32_t(std::to_integer<std::uint8_t>(raw[4 * i + b])) << (8 * b);
        v[i] = static_cast<std::int32_t>(word);
    }
    out = {};
    out.tm_sec = v[0]; out.tm_min = v[1]; out.tm_hour = v[2]; out.tm_mday = v[3]; out.tm_mon = v[4];
    out.tm_year = v[5]; out.tm_wday = v[6]; out.tm_yday = v[7]; out.tm_isdst = v[8];
    return true;
}
bool write_tm(Memory& memory, std::uint32_t address, const std::tm& t) {
    const std::int32_t v[11]{t.tm_sec, t.tm_min, t.tm_hour, t.tm_mday, t.tm_mon, t.tm_year, t.tm_wday, t.tm_yday,
                             t.tm_isdst, static_cast<std::int32_t>(t.tm_gmtoff), 0};
    std::array<std::byte, 44> raw{};
    for (unsigned i = 0; i < 11; ++i)
        for (unsigned b = 0; b < 4; ++b) raw[4 * i + b] = std::byte((static_cast<std::uint32_t>(v[i]) >> (8 * b)) & 255);
    return memory.write(address, raw);
}
}
void install_time_services(ImportDispatcher& dispatcher, std::uint32_t static_tm) {
    for (const bool local : {true, false}) {
        for (const bool reentrant : {false, true}) {
            const std::string name = std::string(local ? "localtime" : "gmtime") + (reentrant ? "_r" : "");
            dispatcher.bind(name, [name, local, reentrant, static_tm](ImportCall& call) -> std::optional<std::string> {
                std::array<std::byte, 4> word{};
                if (!call.runtime().memory.read(call.arg(0), word)) return name + " time_t is unreadable";
                std::int64_t seconds = 0;
                for (unsigned b = 0; b < 4; ++b) seconds |= std::int64_t(std::to_integer<std::uint8_t>(word[b])) << (8 * b);
                seconds = static_cast<std::int32_t>(seconds);
                const std::time_t t = static_cast<std::time_t>(seconds);
                std::tm out{};
                if (!(local ? localtime_r(&t, &out) : gmtime_r(&t, &out))) { call.set_result(0); return std::nullopt; }
                const auto target = reentrant ? call.arg(1) : static_tm;
                if (!write_tm(call.runtime().memory, target, out)) return name + " result buffer is not writable";
                call.set_result(target);
                return std::nullopt;
            });
        }
    }
    // time_t mktime(struct tm*): normalizes the fields in place.
    dispatcher.bind("mktime", [](ImportCall& call) -> std::optional<std::string> {
        std::tm t{};
        if (!read_tm(call.runtime().memory, call.arg(0), t)) return "mktime struct tm is unreadable";
        t.tm_isdst = -1;
        const std::time_t result = std::mktime(&t);
        if (!write_tm(call.runtime().memory, call.arg(0), t)) return "mktime struct tm is not writable";
        call.set_result(static_cast<std::uint32_t>(result == static_cast<std::time_t>(-1) ? 0xffffffffu : static_cast<std::uint32_t>(result)));
        return std::nullopt;
    });
}
}
