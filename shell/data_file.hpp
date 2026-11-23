// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include <cstdint>
#include <string>
#include <string_view>

namespace atlan::shell {
struct DataFileStatus {
    bool ok{};
    std::uint64_t size{};
    std::string message;  // empty when ok, otherwise the explicit rejection reason
};

// Validates a user-placed data file where it lies. The file is never copied or
// loaded whole: only its metadata and a small head and tail window are read.
// `magic`, when non-empty, must match the first bytes. No format is assumed
// otherwise, and a rejected file is reported, never repaired or replaced.
[[nodiscard]] DataFileStatus inspect_data_file(const std::string& path,
                                               std::string_view magic = {},
                                               std::uint64_t min_size = 1) noexcept;
}
