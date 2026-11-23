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
