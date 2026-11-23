// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include "atlan/imports.hpp"
#include <unordered_set>

namespace atlan {
// Single guest thread pthread mutex services. Mutex state is tracked by guest
// address in the host; the guest object is not modified. With one thread, locking
// a held normal mutex can never succeed, so it is reported as a deadlock.
class MutexTable {
public:
    void install(ImportDispatcher& dispatcher);
    [[nodiscard]] bool held(std::uint32_t address) const { return held_.contains(address); }
private:
    std::unordered_set<std::uint32_t> held_;
};
}
