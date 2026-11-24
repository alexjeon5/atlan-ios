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
    [[nodiscard]] std::byte* translate(std::uint64_t address, MemoryAccess access) noexcept;
    [[nodiscard]] bool read(std::uint32_t address, std::span<std::byte> out) noexcept;
    [[nodiscard]] bool write(std::uint32_t address, std::span<const std::byte> data) noexcept;
    // Removes whole pages; unmapped pages inside the range are ignored.
    void unmap(std::uint32_t base, std::uint64_t size);
    void protect(std::uint32_t base, std::uint64_t size, Permissions permissions);
    // Bumped by every map() and protect(); caches of decoded code compare it.
    [[nodiscard]] std::uint64_t generation() const noexcept { return generation_; }

private:
    // A guest page: `data` points at its 4096 bytes, owned here or aliased from external host memory.
    struct Page {
        std::byte* data{};
        Permissions permissions{};
        std::unique_ptr<std::byte[]> owned;  // set for ordinary pages
        std::shared_ptr<void> keepalive;     // set for external pages (shared by all pages of one mapping)
    };
    std::map<std::uint32_t, std::unique_ptr<Page>> pages_;
    std::uint64_t generation_{};
};
}
