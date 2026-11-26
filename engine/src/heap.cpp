// SPDX-License-Identifier: GPL-2.0-or-later
#include "atlan/heap.hpp"
#include "atlan/abi.hpp"
#include <algorithm>
#include <vector>

namespace atlan {
namespace {
constexpr std::uint32_t chunk = 1024 * 1024;
std::uint32_t round_up(std::uint32_t size) {
    return size == 0 ? GuestHeap::alignment : (size + GuestHeap::alignment - 1) & ~(GuestHeap::alignment - 1);
}
}
GuestHeap::GuestHeap(std::uint32_t base, std::uint32_t limit) : base_(base), limit_(limit) {
    if (base % Memory::page_size || limit % chunk || std::uint64_t{base} + limit > (std::uint64_t{1} << 32))
        throw std::invalid_argument("invalid guest heap range");
}
std::uint32_t GuestHeap::allocate(Memory& memory, std::uint32_t request) {
    if (request > limit_ || request > 0xffffffffU - alignment) return 0;
    const std::uint32_t size = round_up(request);
    for (auto it = free_.begin(); it != free_.end(); ++it) {
        if (it->second < size) continue;
        const auto address = it->first, remaining = it->second - size;
        free_.erase(it);
        if (remaining) free_.emplace(address + size, remaining);
        used_.emplace(address, size);
        live_ += size;
        return address;
    }
    if (size > limit_ - top_) return 0;
    while (top_ + size > mapped_) {
        if (mapped_ >= limit_) return 0;
        try { memory.map(base_ + mapped_, chunk, Permissions::read_write); } catch (const std::exception&) { return 0; }
        mapped_ += chunk;
    }
    const auto address = base_ + top_;
    top_ += size;
    used_.emplace(address, size);
    live_ += size;
    return address;
}
bool GuestHeap::release(std::uint32_t address) {
    const auto found = used_.find(address);
    if (found == used_.end()) return false;
    auto size = found->second;
    used_.erase(found);
    live_ -= size;
    auto next = free_.lower_bound(address);
    if (next != free_.end() && address + size == next->first) {
        size += next->second;
        next = free_.erase(next);
    }
    if (next != free_.begin()) {
        const auto previous = std::prev(next);
        if (previous->first + previous->second == address) {
            address = previous->first;
            size += previous->second;
            free_.erase(previous);
        }
    }
    free_.emplace(address, size);
    return true;
}
std::uint32_t GuestHeap::block_size(std::uint32_t address) const noexcept {
    const auto found = used_.find(address);
    return found == used_.end() ? 0 : found->second;
}

void install_heap_services(ImportDispatcher& dispatcher, GuestHeap& heap) {
    // void* malloc(size_t)
    dispatcher.bind("malloc", [&heap](ImportCall& call) -> std::optional<std::string> {
        call.set_result(heap.allocate(call.runtime().memory, call.arg(0)));
        return std::nullopt;
    });
    // char* strdup(const char*): a heap copy including the terminator; NULL when the heap is exhausted.
    dispatcher.bind("strdup", [&heap](ImportCall& call) -> std::optional<std::string> {
        std::string text;
        if (!read_guest_string(call.runtime().memory, call.arg(0), 1U << 24, text)) return "strdup string is unreadable or too long";
        const auto at = heap.allocate(call.runtime().memory, static_cast<std::uint32_t>(text.size() + 1));
        if (at != 0) {
            std::vector<std::byte> bytes(text.size() + 1, std::byte{0});
            for (std::size_t i = 0; i < text.size(); ++i) bytes[i] = static_cast<std::byte>(text[i]);
            if (!call.runtime().memory.write(at, bytes)) return "strdup copy is not writable";
        }
        call.set_result(at);
        return std::nullopt;
    });
    // void free(void*): free(NULL) is a no-op; anything else must be a live block.
    dispatcher.bind("free", [&heap](ImportCall& call) -> std::optional<std::string> {
        if (call.arg(0) != 0 && !heap.release(call.arg(0))) return "free of an address that is not a live heap block";
        return std::nullopt;
    });
    // void* calloc(size_t count, size_t size): overflow returns NULL.
    dispatcher.bind("calloc", [&heap](ImportCall& call) -> std::optional<std::string> {
        const std::uint64_t total = std::uint64_t{call.arg(0)} * call.arg(1);
        if (total > 0xffffffffULL) { call.set_result(0); return std::nullopt; }
        const auto address = heap.allocate(call.runtime().memory, static_cast<std::uint32_t>(total));
        if (address) {
            // A reused block may hold old data; zero the whole block.
            const std::vector<std::byte> zeros(heap.block_size(address));
            if (!call.runtime().memory.write(address, zeros)) return "calloc could not clear a heap block";
        }
        call.set_result(address);
        return std::nullopt;
    });
    // void* realloc(void*, size_t): realloc(NULL,n) is malloc(n); realloc(p,0) frees p and returns NULL.
    dispatcher.bind("realloc", [&heap](ImportCall& call) -> std::optional<std::string> {
        const auto old = call.arg(0), size = call.arg(1);
        auto& memory = call.runtime().memory;
        if (old == 0) { call.set_result(heap.allocate(memory, size)); return std::nullopt; }
        const auto old_size = heap.block_size(old);
        if (old_size == 0) return "realloc of an address that is not a live heap block";
        if (size == 0) { (void)heap.release(old); call.set_result(0); return std::nullopt; }
        if (round_up(size) <= old_size) { call.set_result(old); return std::nullopt; }
        const auto fresh = heap.allocate(memory, size);
        if (fresh == 0) { call.set_result(0); return std::nullopt; }  // old block stays valid
        std::vector<std::byte> data(old_size);
        if (!memory.read(old, data) || !memory.write(fresh, data)) return "realloc could not copy a heap block";
        (void)heap.release(old);
        call.set_result(fresh);
        return std::nullopt;
    });
}
}
