// SPDX-License-Identifier: GPL-2.0-or-later
#include "atlan/mmap.hpp"
#include <algorithm>
#include <cstring>
#include <vector>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

namespace atlan {
std::uint32_t GuestMmap::reserve(std::uint32_t length) {
    if (length == 0 || length > limit_) return 0;
    const std::uint64_t size = (std::uint64_t{length} + Memory::page_size - 1) & ~std::uint64_t{Memory::page_size - 1};
    std::uint64_t at = base_;
    for (const auto& [start, used] : used_) {
        if (start >= at + size) break;
        at = std::uint64_t{start} + used;
    }
    if (at + size > std::uint64_t{base_} + limit_) return 0;
    used_[static_cast<std::uint32_t>(at)] = static_cast<std::uint32_t>(size);
    return static_cast<std::uint32_t>(at);
}
std::uint32_t GuestMmap::map(Memory& memory, std::uint32_t length, Permissions permissions) {
    const auto at = reserve(length);
    if (at == 0) return 0;
    try { memory.map(at, used_[at], permissions); } catch (const std::invalid_argument&) { used_.erase(at); return 0; }
    return at;
}
std::uint32_t GuestMmap::map_file(Memory& memory, int host_fd, bool shared, std::uint32_t length, std::uint32_t offset, Permissions permissions) {
    struct ::stat info{};
    if (::fstat(host_fd, &info) != 0 || !S_ISREG(info.st_mode)) return 0xffffffffU;
    const std::uint64_t size = static_cast<std::uint64_t>(info.st_size);
    const std::uint64_t available = size > offset ? size - offset : 0;
    const std::uint64_t whole_pages = std::min<std::uint64_t>(length, available) / Memory::page_size;  // pages fully inside the file
    const auto at = reserve(length);
    if (at == 0) return 0;
    const std::uint64_t total = used_[at];
    try {
        if (whole_pages != 0) {
            const std::uint64_t host_page = static_cast<std::uint64_t>(::sysconf(_SC_PAGESIZE));
            const std::uint64_t aligned = offset & ~(host_page - 1), delta = offset - aligned;
            const std::uint64_t span = delta + whole_pages * Memory::page_size;
            const int prot = shared && permissions != Permissions::read_write ? PROT_READ : PROT_READ | PROT_WRITE;
            void* host = ::mmap(nullptr, span, prot, shared ? MAP_SHARED : MAP_PRIVATE, host_fd, static_cast<off_t>(aligned));
            if (host == MAP_FAILED) { used_.erase(at); return 0xffffffffU; }
            std::shared_ptr<void> keepalive(host, [span](void* p) { ::munmap(p, span); });
            memory.map_external(at, whole_pages * Memory::page_size, permissions, static_cast<std::byte*>(host) + delta, std::move(keepalive));
        }
        if (total > whole_pages * Memory::page_size) {  // partial last page and anything past end of file: private zero pages
            const auto rest = static_cast<std::uint32_t>(at + whole_pages * Memory::page_size);
            std::vector<std::byte> tail(total - whole_pages * Memory::page_size);
            const std::uint64_t tail_offset = std::uint64_t{offset} + whole_pages * Memory::page_size;
            if (tail_offset < size) {
                const auto want = std::min<std::uint64_t>(tail.size(), size - tail_offset);
                std::uint64_t done = 0;
                while (done < want) {
                    const auto got = ::pread(host_fd, tail.data() + done, want - done, static_cast<off_t>(tail_offset + done));
                    if (got <= 0) break;
                    done += static_cast<std::uint64_t>(got);
                }
            }
            memory.map(rest, tail.size(), permissions, tail);
        }
    } catch (const std::invalid_argument&) {
        memory.unmap(at, total);
        used_.erase(at);
        return 0;
    }
    return at;
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
        constexpr std::uint32_t map_shared = 1;
        const bool file = (flags == map_private || flags == map_shared) && fd != 0xffffffffU && offset % Memory::page_size == 0;
        if ((!anonymous && !file) || addr != 0 || (file && !host_fd_))
            return "mmap supports only MAP_PRIVATE|MAP_ANONYMOUS or file mappings without a hint (flags=" + std::to_string(flags) +
                   " prot=" + std::to_string(prot) + " fd=" + std::to_string(static_cast<std::int32_t>(fd)) + (file && !host_fd_ ? ", no file reader" : "") + ")";
        if (prot != 1 && prot != 3) return "mmap prot " + std::to_string(prot) + " is not supported";
        const auto permissions = prot == 1 ? Permissions::read : Permissions::read_write;
        std::uint32_t at = 0;
        if (anonymous) at = map(call.runtime().memory, length, permissions);
        else {
            const auto host_fd = host_fd_(fd);
            if (!host_fd) { call.set_result(0xffffffffU); return std::nullopt; }  // EBADF: MAP_FAILED
            if (flags == map_shared && permissions == Permissions::read_write && !host_fd->second)
                return "mmap MAP_SHARED with PROT_WRITE needs a descriptor opened for writing";
            at = map_file(call.runtime().memory, host_fd->first, flags == map_shared, length, offset, permissions);
            if (at == 0xffffffffU) return "mmap of a file could not be mapped on the host";
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
