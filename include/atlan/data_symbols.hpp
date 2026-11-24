// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include "atlan/memory.hpp"
#include <optional>
#include <string_view>

namespace atlan {
// Read-only data objects the guest imports from Bionic (not functions).
// `_toupper_tab_` is a `const short*` variable (the guest loads the pointer, then indexes it) that points
// at `short[257]`: index 0 is EOF (-1), index 1+c is toupper(c) in the C locale. (Verified against the
// guest's memicmp: GOT -> variable -> table.)
class BionicData {
public:
    static constexpr std::uint32_t base = 0xe3000000;
    // Address of the imported data symbol, or nullopt if it is not provided here.
    [[nodiscard]] static std::optional<std::uint32_t> symbol(std::string_view name) noexcept {
        if (name == "_toupper_tab_") return base;
        return std::nullopt;
    }
    // Maps the tables read-only; throws std::invalid_argument if the page is taken.
    static void map(Memory& memory);
};
}
