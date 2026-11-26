// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include <array>
#include <cstddef>
#include <cstdint>
#include <map>
#include <memory>
#include <span>

namespace atlan {
enum class MemoryAccess : std::uint8_t { read = 1, write = 2, execute = 4 };
enum class Permissions : std::uint8_t { read = 1, read_write = 3, read_execute = 5 };

// All regions cover whole guest pages, independent of the host's page size.
class Memory {
public:
    static constexpr std::uint32_t page_size = 4096;
    void map(std::uint32_t base, std::uint64_t size, Permissions permissions,
             std::span<const std::byte> initial = {});
    // Maps pages that alias caller-owned host memory (for example a private file mapping) instead of
    // copying it. `host` must point at `size` bytes valid for read and write until the last of these pages is
    // unmapped, when `keepalive` is released. Overlap and alignment rules are those of map().
    void map_external(std::uint32_t base, std::uint64_t size, Permissions permissions, std::byte* host,
                      std::shared_ptr<void> keepalive);
    [[nodiscard]] std::byte* translate(std::uint64_t address, MemoryAccess access) noexcept {
        if (address > UINT32_MAX) return nullptr;
        const auto value = static_cast<std::uint32_t>(address);
        const Leaf* leaf = table_[value >> 22].get();
        if (leaf == nullptr) return nullptr;
        const Slot& slot = (*leaf)[(value >> 12) & 1023U];
        if ((slot.permissions & static_cast<unsigned>(access)) == 0) return nullptr;
        return slot.data + (value & (page_size - 1U));
    }
    [[nodiscard]] bool read(std::uint32_t address, std::span<std::byte> out) noexcept;
    [[nodiscard]] bool write(std::uint32_t address, std::span<const std::byte> data) noexcept;
    // memset and memmove on guest memory without a host-side copy. Like write(), nothing changes unless every
    // page of the range translates (for move(): the source for reading and the destination for writing).
    [[nodiscard]] bool fill(std::uint32_t address, std::byte value, std::uint64_t count) noexcept;
    [[nodiscard]] bool move(std::uint32_t destination, std::uint32_t source, std::uint64_t count) noexcept;
    // The host bytes from `address` to the end of its guest page, or an empty span when the page does not
    // translate for `access`. For bounded scans (strlen, memcmp) that would otherwise translate per byte.
    [[nodiscard]] std::span<std::byte> page_span(std::uint32_t address, MemoryAccess access) noexcept {
        std::byte* host = translate(address, access);
        if (host == nullptr) return {};
        return {host, page_size - (address & (page_size - 1U))};
    }
    // Removes whole pages; unmapped pages inside the range are ignored.
    void unmap(std::uint32_t base, std::uint64_t size);
    void protect(std::uint32_t base, std::uint64_t size, Permissions permissions);
    // Bumped by every map() and protect(); caches of decoded code compare it.
    [[nodiscard]] std::uint64_t generation() const noexcept { return generation_; }
    // Changes whenever a page that is or was executable is mapped, unmapped or re-protected. Values are unique
    // across all Memory objects of the process, so a cache keyed by it can also tell two address spaces apart.
    [[nodiscard]] std::uint64_t code_generation() const noexcept { return code_generation_; }

private:
    [[nodiscard]] bool translates(std::uint32_t address, std::uint64_t count, MemoryAccess access) noexcept;
    // A guest page: `data` points at its 4096 bytes, owned here or aliased from external host memory.
    struct Page {
        std::byte* data{};
        Permissions permissions{};
        std::unique_ptr<std::byte[]> owned;  // set for ordinary pages
        std::shared_ptr<void> keepalive;     // set for external pages (shared by all pages of one mapping)
    };
    std::map<std::uint32_t, std::unique_ptr<Page>> pages_;
    // Two-level lookup table over pages_ (10 + 10 address bits), kept in step by every mutation; translate()
    // reads only this. A slot with permissions 0 is unmapped.
    struct Slot {
        std::byte* data{};
        std::uint8_t permissions{};
    };
    using Leaf = std::array<Slot, 1024>;
    void set_slot(std::uint32_t page_address, std::byte* data, std::uint8_t permissions);
    static std::uint64_t next_code_generation() noexcept;
    std::array<std::unique_ptr<Leaf>, 1024> table_;
    std::uint64_t generation_{};
    std::uint64_t code_generation_{next_code_generation()};
};
}
