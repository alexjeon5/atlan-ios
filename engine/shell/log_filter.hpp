// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include <cstdint>
#include <mutex>
#include <string>
#include <string_view>
#include <unordered_map>

namespace atlan::shell {
// Keeps an on-screen log readable when the guest repeats a line every frame: each distinct line passes
// `limit` times, the last time marked, and is dropped afterwards. Lines starting with one of the `always`
// prefixes pass every time. Safe to call from several threads.
class RepeatFilter {
public:
    explicit RepeatFilter(std::uint32_t limit = 3) : limit_(limit == 0 ? 1 : limit) {}
    // Returns true when `line` should be shown; it may be extended with a note on its last showing.
    [[nodiscard]] bool admit(std::string& line) {
        if (std::string_view(line).starts_with("perf ")) return true;
        const std::lock_guard lock(mutex_);
        if (seen_.size() >= max_lines && !seen_.contains(line)) seen_.clear();  // bounded memory; lines may show again
        const std::uint32_t count = ++seen_[line];
        if (count > limit_) { --seen_[line]; return false; }
        if (count == limit_) line += "  (repeats; further copies hidden)";
        return true;
    }
private:
    static constexpr std::size_t max_lines = 4096;
    std::uint32_t limit_;
    std::mutex mutex_;
    std::unordered_map<std::string, std::uint32_t> seen_;
};
}
