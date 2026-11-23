// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include "atlan/imports.hpp"
#include <map>

namespace atlan {
// Guest heap for malloc-family services. Block metadata lives in the host, never
// in guest memory, so guest overruns cannot corrupt allocator state. Guest pages
// are mapped read-write on demand, contiguously from `base`, up to `limit` bytes.
class GuestHeap {
public:
    static constexpr std::uint32_t alignment = 16;
    explicit GuestHeap(std::uint32_t base = 0x80000000, std::uint32_t limit = 256U * 1024 * 1024);
    // Returns 0 when the request cannot be satisfied (malloc's ENOMEM result).
    [[nodiscard]] std::uint32_t allocate(Memory& memory, std::uint32_t size);
    // False for an address that is not the start of a live block (invalid or double free).
    [[nodiscard]] bool release(std::uint32_t address);
    // Size of the live block starting at address, or 0.
    [[nodiscard]] std::uint32_t block_size(std::uint32_t address) const noexcept;
    [[nodiscard]] std::uint64_t live_bytes() const noexcept { return live_; }
private:
    std::uint32_t base_, limit_, mapped_{};
    std::uint32_t top_{};      // offset of the first never-used byte
    std::uint64_t live_{};
    std::map<std::uint32_t, std::uint32_t> used_;  // address -> block size (rounded)
    std::map<std::uint32_t, std::uint32_t> free_;  // address -> block size, coalesced
};

// malloc, free, calloc, realloc as typed services over `heap`, which must outlive
// the dispatcher.
void install_heap_services(ImportDispatcher& dispatcher, GuestHeap& heap);
}
