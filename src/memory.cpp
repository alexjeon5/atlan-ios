// SPDX-License-Identifier: GPL-2.0-or-later
#include "atlan/memory.hpp"
#include <algorithm>
#include <stdexcept>

namespace atlan {
void Memory::map(std::uint32_t base, std::uint64_t size, Permissions permissions,
                 std::span<const std::byte> initial) {
    const auto end = std::uint64_t{base} + size;
    if (size == 0 || size > (std::uint64_t{1} << 32) ||
        (base % page_size) != 0 || (size % page_size) != 0 ||
        end > (std::uint64_t{1} << 32) || initial.size() > size ||
        (permissions != Permissions::read && permissions != Permissions::read_write &&
         permissions != Permissions::read_execute))
        throw std::invalid_argument("invalid ARM32 mapping");
    for (std::uint64_t at = base; at < end; at += page_size)
        if (pages_.contains(static_cast<std::uint32_t>(at)))
            throw std::invalid_argument("overlapping ARM32 mapping");
    // Stage allocations so a failed mapping leaves the address space unchanged.
    decltype(pages_) staged;
    for (std::uint64_t offset = 0; offset < size; offset += page_size) {
        auto page = std::make_unique<Page>();
        page->permissions = permissions;
        page->owned = std::make_unique<std::byte[]>(page_size);  // value-initialised: zero filled
        page->data = page->owned.get();
        if (offset < initial.size()) {
            const auto count = std::min<std::size_t>(page_size, initial.size() - offset);
            std::copy_n(initial.data() + offset, count, page->data);
        }
        staged.emplace(static_cast<std::uint32_t>(base + offset), std::move(page));
    }
    pages_.merge(staged);
    ++generation_;
}

void Memory::map_external(std::uint32_t base, std::uint64_t size, Permissions permissions, std::byte* host,
                          std::shared_ptr<void> keepalive) {
    const auto end = std::uint64_t{base} + size;
    if (host == nullptr || size == 0 || size > (std::uint64_t{1} << 32) || (base % page_size) != 0 || (size % page_size) != 0 ||
        end > (std::uint64_t{1} << 32) ||
        (permissions != Permissions::read && permissions != Permissions::read_write && permissions != Permissions::read_execute))
        throw std::invalid_argument("invalid ARM32 external mapping");
    for (std::uint64_t at = base; at < end; at += page_size)
        if (pages_.contains(static_cast<std::uint32_t>(at))) throw std::invalid_argument("overlapping ARM32 mapping");
    decltype(pages_) staged;
    for (std::uint64_t offset = 0; offset < size; offset += page_size) {
        auto page = std::make_unique<Page>();
        page->permissions = permissions;
        page->data = host + offset;
        page->keepalive = keepalive;
        staged.emplace(static_cast<std::uint32_t>(base + offset), std::move(page));
    }
    pages_.merge(staged);
    ++generation_;
}

std::byte* Memory::translate(std::uint64_t address, MemoryAccess access) noexcept {
    if (address > UINT32_MAX) return nullptr;
    const auto value = static_cast<std::uint32_t>(address);
    const auto page = pages_.find(value & ~(page_size - 1U));
    if (page == pages_.end() ||
        (static_cast<unsigned>(page->second->permissions) & static_cast<unsigned>(access)) == 0)
        return nullptr;
    return page->second->data + (value & (page_size - 1U));
}

bool Memory::read(std::uint32_t address, std::span<std::byte> out) noexcept {
    if (out.size() > (std::uint64_t{1} << 32) - address) return false;
    // Check every page before exposing any partial result.
    for (std::size_t i = 0; i < out.size();) {
        const auto at = std::uint64_t{address} + i;
        if (!translate(at, MemoryAccess::read)) return false;
        i += std::min<std::size_t>(page_size - (at % page_size), out.size() - i);
    }
    for (std::size_t i = 0; i < out.size(); ++i)
        out[i] = *translate(std::uint64_t{address} + i, MemoryAccess::read);
    return true;
}

bool Memory::write(std::uint32_t address, std::span<const std::byte> data) noexcept {
    if (data.size() > (std::uint64_t{1} << 32) - address) return false;
    for (std::size_t i = 0; i < data.size();) {
        const auto at = std::uint64_t{address} + i;
        if (!translate(at, MemoryAccess::write)) return false;
        i += std::min<std::size_t>(page_size - (at % page_size), data.size() - i);
    }
    for (std::size_t i = 0; i < data.size();) {
        const auto at = std::uint64_t{address} + i;
        const auto count = std::min<std::size_t>(page_size - (at % page_size), data.size() - i);
        std::copy_n(data.data() + i, count, translate(at, MemoryAccess::write));
        i += count;
    }
    return true;
}

void Memory::protect(std::uint32_t base, std::uint64_t size, Permissions permissions) {
    if (size == 0 || size > (std::uint64_t{1} << 32) - base ||
        base % page_size != 0 || size % page_size != 0 ||
        (permissions != Permissions::read && permissions != Permissions::read_write &&
         permissions != Permissions::read_execute))
        throw std::invalid_argument("invalid ARM32 protection range");
    const auto end = std::uint64_t{base} + size;
    for (std::uint64_t at = base; at < end; at += page_size)
        if (!pages_.contains(static_cast<std::uint32_t>(at)))
            throw std::invalid_argument("unmapped ARM32 protection range");
    for (std::uint64_t at = base; at < end; at += page_size)
        pages_.at(static_cast<std::uint32_t>(at))->permissions = permissions;
    ++generation_;
}
void Memory::unmap(std::uint32_t base, std::uint64_t size) {
    if (size == 0 || size > (std::uint64_t{1} << 32) - base || base % page_size != 0 || size % page_size != 0)
        throw std::invalid_argument("invalid ARM32 unmap range");
    for (std::uint64_t at = base; at < std::uint64_t{base} + size; at += page_size)
        pages_.erase(static_cast<std::uint32_t>(at));
}
}
