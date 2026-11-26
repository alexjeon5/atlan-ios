// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include "atlan/imports.hpp"
#include <array>
#include <vector>

namespace atlan {
// C++ static-object lifetime registry behind __aeabi_atexit, __cxa_atexit and
// __cxa_finalize. Entries hold guest addresses only; destructors run through the
// runtime with bounded execution, never through host atexit. The registry must
// outlive every dispatcher that has it installed.
class ObjectLifetime {
public:
    struct Entry {
        std::uint32_t function{};  // guest address, Thumb bit preserved
        std::uint32_t object{};    // guest argument passed in r0
        std::uint32_t dso{};       // __dso_handle identity (guest address)
    };
    static constexpr std::size_t max_entries = 65536;

    void install(ImportDispatcher& dispatcher);
    [[nodiscard]] std::optional<std::string> add(Entry entry);
    // Runs matching entries newest first; each entry is removed before it runs.
    // dso == 0 selects all entries. Stops at the first callback failure.
    [[nodiscard]] std::optional<std::string> finalize(ImportDispatcher& dispatcher,
                                                      Runtime& runtime, std::uint32_t dso);
    [[nodiscard]] const std::vector<Entry>& entries() const noexcept { return entries_; }
private:
    std::vector<Entry> entries_;
};
}
