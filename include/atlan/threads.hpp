// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include "atlan/imports.hpp"
#include <unordered_map>
#include <vector>

namespace atlan {
// Single guest thread pthread mutex services. State is tracked by guest address
// in the host. A mutexattr word in guest memory holds its type (0 normal, 1
// recursive, 2 error-checking, as in Bionic). With one thread, locking a held
// normal mutex can never succeed, so it is reported as a deadlock.
class MutexTable {
public:
    void install(ImportDispatcher& dispatcher);
    [[nodiscard]] bool held(std::uint32_t address) const {
        const auto found = mutexes_.find(address);
        return found != mutexes_.end() && found->second.count != 0;
    }
private:
    struct State { std::uint32_t type{}; std::uint32_t count{}; };
    std::unordered_map<std::uint32_t, State> mutexes_;  // absent: statically initialized, normal
};

// Thread-specific data for the single guest thread. Destructors are recorded but
// never run, since the guest thread never exits.
class ThreadKeys {
public:
    static constexpr std::uint32_t max_keys = 128;
    void install(ImportDispatcher& dispatcher);
private:
    struct Key { bool live{}; std::uint32_t value{}; std::uint32_t destructor{}; };
    std::vector<Key> keys_;
};
}
