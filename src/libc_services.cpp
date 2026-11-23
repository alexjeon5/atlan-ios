// SPDX-License-Identifier: GPL-2.0-or-later
#include "atlan/libc_services.hpp"
#include <array>
#include <chrono>
#include <vector>

namespace atlan {
namespace {
constexpr std::uint32_t max_block = 64U * 1024 * 1024;  // bound one service call
}
void install_libc_services(ImportDispatcher& dispatcher) {
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
}
}
