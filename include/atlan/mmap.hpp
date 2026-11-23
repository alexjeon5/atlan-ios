// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include "atlan/imports.hpp"
#include <map>

namespace atlan {
// Anonymous private mmap/munmap over a dedicated guest address arena. Only
// MAP_PRIVATE|MAP_ANONYMOUS (fd -1) with PROT_READ or PROT_READ|PROT_WRITE is
// served; file-backed, fixed, PROT_NONE and executable requests stop explicitly.
// Failure to find space returns MAP_FAILED. The arena must outlive the dispatcher.
class GuestMmap {
public:
    explicit GuestMmap(std::uint32_t base = 0x90000000, std::uint32_t limit = 512U * 1024 * 1024)
        : base_(base), limit_(limit) {}
    void install(ImportDispatcher& dispatcher);
    // Returns 0 if no space; length is rounded up to whole pages.
    [[nodiscard]] std::uint32_t map(Memory& memory, std::uint32_t length, Permissions permissions);
    // False unless [address, address+length) lies inside live mappings made by map().
    [[nodiscard]] bool unmap(Memory& memory, std::uint32_t address, std::uint32_t length);
    [[nodiscard]] std::uint64_t live_bytes() const noexcept;
private:
    std::uint32_t base_, limit_;
    std::map<std::uint32_t, std::uint32_t> used_;  // start -> page-rounded size
};
}
