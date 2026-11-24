// SPDX-License-Identifier: GPL-2.0-or-later
#include "atlan/mmap.hpp"
#include <vector>

namespace atlan {
std::uint32_t GuestMmap::map(Memory& memory, std::uint32_t length, Permissions permissions) {
    if (length == 0 || length > limit_) return 0;
    const std::uint64_t size = (std::uint64_t{length} + Memory::page_size - 1) & ~std::uint64_t{Memory::page_size - 1};
    std::uint64_t at = base_;
    for (const auto& [start, used] : used_) {
        if (start >= at + size) break;
        at = std::uint64_t{start} + used;
    }
    if (at + size > std::uint64_t{base_} + limit_) return 0;
    try { memory.map(static_cast<std::uint32_t>(at), size, permissions); } catch (const std::invalid_argument&) { return 0; }
    used_[static_cast<std::uint32_t>(at)] = static_cast<std::uint32_t>(size);
    return static_cast<std::uint32_t>(at);
}
bool GuestMmap::unmap(Memory& memory, std::uint32_t address, std::uint32_t length) {
    if (length == 0 || address % Memory::page_size != 0) return false;
    const std::uint64_t size = (std::uint64_t{length} + Memory::page_size - 1) & ~std::uint64_t{Memory::page_size - 1};
    auto it = used_.upper_bound(address);
    if (it == used_.begin()) return false;
    --it;
    if (address < it->first || address + size > std::uint64_t{it->first} + it->second) return false;
    const auto start = it->first, total = it->second;
    memory.unmap(address, size);
    used_.erase(it);
    if (address > start) used_[start] = address - start;
    if (address + size < std::uint64_t{start} + total)
        used_[static_cast<std::uint32_t>(address + size)] = static_cast<std::uint32_t>(std::uint64_t{start} + total - address - size);
    return true;
}
std::uint64_t GuestMmap::live_bytes() const noexcept {
    std::uint64_t total = 0;
    for (const auto& [start, size] : used_) total += size;
    return total;
}
void GuestMmap::install(ImportDispatcher& dispatcher) {
    // void* mmap(void* addr, size_t length, int prot, int flags, int fd, off_t offset)
    dispatcher.bind("mmap", [this](ImportCall& call) -> std::optional<std::string> {
        std::uint32_t fd{}, offset{};
        if (!call.read_arg(4, fd) || !call.read_arg(5, offset)) return "mmap stack arguments are unreadable";
        const auto addr = call.arg(0), length = call.arg(1), prot = call.arg(2), flags = call.arg(3);
        constexpr std::uint32_t map_private = 2, map_anonymous = 0x20;
        const bool anonymous = flags == (map_private | map_anonymous) && fd == 0xffffffffU && offset == 0;
        const bool file = flags == map_private && fd != 0xffffffffU && offset % Memory::page_size == 0;
        if ((!anonymous && !file) || addr != 0 || (file && !reader_))
            return "mmap supports only private mappings without a hint (flags=" + std::to_string(flags) +
                   " fd=" + std::to_string(static_cast<std::int32_t>(fd)) + (file && !reader_ ? ", no file reader" : "") + ")";
        if (prot != 1 && prot != 3) return "mmap prot " + std::to_string(prot) + " is not supported";
        const auto permissions = prot == 1 ? Permissions::read : Permissions::read_write;
        std::uint32_t at = 0;
        if (anonymous) at = map(call.runtime().memory, length, permissions);
        else {
            if (length == 0 || length > limit_) { call.set_result(0xffffffffU); return std::nullopt; }
            std::vector<std::byte> content(length);
            const auto got = reader_(fd, offset, content);
            if (!got) { call.set_result(0xffffffffU); return std::nullopt; }  // EBADF: MAP_FAILED
            at = map(call.runtime().memory, length, Permissions::read_write);
            if (at != 0) {
                if (!call.runtime().memory.write(at, std::span<const std::byte>(content.data(), *got)))
                    return "mmap could not fill the file mapping";
                if (permissions != Permissions::read_write) {
                    call.runtime().memory.protect(at, (std::uint64_t{length} + Memory::page_size - 1) & ~std::uint64_t{Memory::page_size - 1}, permissions);
                }
            }
        }
        call.set_result(at != 0 ? at : 0xffffffffU);  // MAP_FAILED
        return std::nullopt;
    });
    // int munmap(void* addr, size_t length)
    dispatcher.bind("munmap", [this](ImportCall& call) -> std::optional<std::string> {
        if (!unmap(call.runtime().memory, call.arg(0), call.arg(1))) return "munmap range is not inside a live anonymous mapping";
        call.set_result(0);
        return std::nullopt;
    });
}
}
