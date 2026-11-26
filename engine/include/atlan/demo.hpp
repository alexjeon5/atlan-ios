// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include "atlan/runtime.hpp"
namespace atlan {
struct DemoResult {
    std::uint32_t value{};
    Result execution{};
};
// Synthetic ARM code, with no application or external binary dependency.
[[nodiscard]] DemoResult run_demo();
}
