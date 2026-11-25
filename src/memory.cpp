// SPDX-License-Identifier: GPL-2.0-or-later
#include "atlan/memory.hpp"
#include <algorithm>
#include <atomic>
#include <cstring>
#include <stdexcept>

namespace atlan {
std::uint64_t Memory::next_code_generation() noexcept {
    static std::atomic<std::uint64_t> counter{0};
    return counter.fetch_add(1, std::memory_order_relaxed) + 1;
}
void Memory::set_slot(std::uint32_t page_address, std::byte* data, std::uint8_t permissions) {
    auto& leaf = table_[page_address >> 22];
    if (!leaf) {
        if (permissions == 0) return;
        leaf = std::make_unique<Leaf>();
    }
    (*leaf)[(page_address >> 12) & 1023U] = {data, permissions};
}
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
    // Allocate the lookup leaves before committing, so a failure still leaves the address space unchanged.
    for (std::uint64_t at = base; at < end; at += std::uint64_t{1} << 22)
        if (auto& leaf = table_[at >> 22]; !leaf) leaf = std::make_unique<Leaf>();
    if (auto& leaf = table_[(end - 1) >> 22]; !leaf) leaf = std::make_unique<Leaf>();
    for (const auto& [at, page] : staged) set_slot(at, page->data, static_cast<std::uint8_t>(permissions));
    pages_.merge(staged);
    ++generation_;
    if (permissions == Permissions::read_execute) code_generation_ = next_code_generation();
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
    // Allocate the lookup leaves before committing, so a failure still leaves the address space unchanged.
    for (std::uint64_t at = base; at < end; at += std::uint64_t{1} << 22)
        if (auto& leaf = table_[at >> 22]; !leaf) leaf = std::make_unique<Leaf>();
    if (auto& leaf = table_[(end - 1) >> 22]; !leaf) leaf = std::make_unique<Leaf>();
    for (const auto& [at, page] : staged) set_slot(at, page->data, static_cast<std::uint8_t>(permissions));
    pages_.merge(staged);
    ++generation_;
    if (permissions == Permissions::read_execute) code_generation_ = next_code_generation();
}

bool Memory::translates(std::uint32_t address, std::uint64_t count, MemoryAccess access) noexcept {
    if (count > (std::uint64_t{1} << 32) - address) return false;
    for (std::uint64_t i = 0; i < count;) {
        const auto at = std::uint64_t{address} + i;
        if (!translate(at, access)) return false;
        i += std::min<std::uint64_t>(page_size - (at % page_size), count - i);
    }
    return true;
}

bool Memory::read(std::uint32_t address, std::span<std::byte> out) noexcept {
    if (!out.empty() && (address & (page_size - 1U)) + out.size() <= page_size) {  // the common case: one page
        const std::byte* host = translate(address, MemoryAccess::read);
        if (host == nullptr) return false;
        std::memcpy(out.data(), host, out.size());
        return true;
    }
    // Check every page before exposing any partial result.
    if (!translates(address, out.size(), MemoryAccess::read)) return false;
    for (std::size_t i = 0; i < out.size();) {
        const auto at = std::uint64_t{address} + i;
        const auto count = std::min<std::size_t>(page_size - (at % page_size), out.size() - i);
        std::copy_n(translate(at, MemoryAccess::read), count, out.data() + i);
        i += count;
    }
    return true;
}

bool Memory::fill(std::uint32_t address, std::byte value, std::uint64_t count) noexcept {
    if (!translates(address, count, MemoryAccess::write)) return false;
    for (std::uint64_t i = 0; i < count;) {
        const auto at = std::uint64_t{address} + i;
        const auto chunk = std::min<std::uint64_t>(page_size - (at % page_size), count - i);
        std::memset(translate(at, MemoryAccess::write), std::to_integer<int>(value), static_cast<std::size_t>(chunk));
        i += chunk;
    }
    return true;
}

bool Memory::move(std::uint32_t destination, std::uint32_t source, std::uint64_t count) noexcept {
    if (!translates(source, count, MemoryAccess::read) || !translates(destination, count, MemoryAccess::write)) return false;
    // Chunks never cross a page of either range. Ascending order is safe when the destination is below the
    // source, descending when it is above; memmove covers the overlap inside one chunk.
    if (destination <= source) {
        for (std::uint64_t i = 0; i < count;) {
            const auto from = std::uint64_t{source} + i, to = std::uint64_t{destination} + i;
            const auto chunk = std::min<std::uint64_t>({page_size - (from % page_size), page_size - (to % page_size), count - i});
            std::memmove(translate(to, MemoryAccess::write), translate(from, MemoryAccess::read), static_cast<std::size_t>(chunk));
            i += chunk;
        }
    } else {
        for (std::uint64_t left = count; left != 0;) {
            const auto from_end = std::uint64_t{source} + left, to_end = std::uint64_t{destination} + left;
            const auto chunk = std::min<std::uint64_t>({((from_end - 1) % page_size) + 1, ((to_end - 1) % page_size) + 1, left});
            std::memmove(translate(to_end - chunk, MemoryAccess::write), translate(from_end - chunk, MemoryAccess::read),
                         static_cast<std::size_t>(chunk));
            left -= chunk;
        }
    }
    return true;
}

bool Memory::write(std::uint32_t address, std::span<const std::byte> data) noexcept {
    if (!data.empty() && (address & (page_size - 1U)) + data.size() <= page_size) {
        std::byte* host = translate(address, MemoryAccess::write);
        if (host == nullptr) return false;
        std::memcpy(host, data.data(), data.size());
        return true;
    }
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
    bool code = permissions == Permissions::read_execute;
    for (std::uint64_t at = base; at < end; at += page_size) {
        auto& page = *pages_.at(static_cast<std::uint32_t>(at));
        code = code || page.permissions == Permissions::read_execute;
        page.permissions = permissions;
        set_slot(static_cast<std::uint32_t>(at), page.data, static_cast<std::uint8_t>(permissions));
    }
    ++generation_;
    if (code) code_generation_ = next_code_generation();
}
void Memory::unmap(std::uint32_t base, std::uint64_t size) {
    if (size == 0 || size > (std::uint64_t{1} << 32) - base || base % page_size != 0 || size % page_size != 0)
        throw std::invalid_argument("invalid ARM32 unmap range");
    bool code = false;
    for (std::uint64_t at = base; at < std::uint64_t{base} + size; at += page_size) {
        const auto page = pages_.find(static_cast<std::uint32_t>(at));
        if (page == pages_.end()) continue;
        code = code || page->second->permissions == Permissions::read_execute;
        set_slot(static_cast<std::uint32_t>(at), nullptr, 0);
        pages_.erase(page);
    }
    if (code) code_generation_ = next_code_generation();
}
}
