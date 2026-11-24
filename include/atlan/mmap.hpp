// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include "atlan/imports.hpp"
#include <functional>
#include <map>

namespace atlan {
// Private mmap/munmap over a dedicated guest address arena. Only
// MAP_PRIVATE|MAP_ANONYMOUS (fd -1) with PROT_READ or PROT_READ|PROT_WRITE is
// served; file-backed, fixed, PROT_NONE and executable requests stop explicitly.
// Failure to find space returns MAP_FAILED. The arena must outlive the dispatcher.
class GuestMmap {
public:
    explicit GuestMmap(std::uint32_t base = 0x90000000, std::uint32_t limit = 768U * 1024 * 1024)
        : base_(base), limit_(limit) {}
    // Resolves a guest fd to a host descriptor that may be memory-mapped (nullopt: bad fd). Without one,
    // file-backed requests stop explicitly. MAP_PRIVATE file mappings alias the host file pages lazily
    // (copy-on-write; nothing is written back), so they cost memory only for the pages the guest touches. MAP_SHARED
    // maps the same pages write-through, writable only when the guest opened the file for writing.
    using HostDescriptor = std::function<std::optional<std::pair<int, bool>>(std::uint32_t)>;  // (host fd, writable)
    void set_host_descriptors(HostDescriptor lookup) { host_fd_ = std::move(lookup); }
    void install(ImportDispatcher& dispatcher);
    // Returns 0 if no space; length is rounded up to whole pages.
    [[nodiscard]] std::uint32_t map(Memory& memory, std::uint32_t length, Permissions permissions);
    // False unless [address, address+length) lies inside live mappings made by map().
    [[nodiscard]] bool unmap(Memory& memory, std::uint32_t address, std::uint32_t length);
    [[nodiscard]] std::uint64_t live_bytes() const noexcept;
private:
    HostDescriptor host_fd_;
    // 0: no space; 0xffffffff: the host refused the mapping.
    [[nodiscard]] std::uint32_t map_file(Memory& memory, int host_fd, bool shared, std::uint32_t length, std::uint32_t offset, Permissions permissions);
    [[nodiscard]] std::uint32_t reserve(std::uint32_t length);  // finds and records arena space; 0 if none
    std::uint32_t base_, limit_;
    std::map<std::uint32_t, std::uint32_t> used_;  // start -> page-rounded size
};
}
