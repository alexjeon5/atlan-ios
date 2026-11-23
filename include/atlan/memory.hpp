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
    [[nodiscard]] std::byte* translate(std::uint64_t address, MemoryAccess access) noexcept;
    [[nodiscard]] bool read(std::uint32_t address, std::span<std::byte> out) noexcept;
    [[nodiscard]] bool write(std::uint32_t address, std::span<const std::byte> data) noexcept;
    void protect(std::uint32_t base, std::uint64_t size, Permissions permissions);
    // Bumped by every map() and protect(); caches of decoded code compare it.
    [[nodiscard]] std::uint64_t generation() const noexcept { return generation_; }

private:
    struct alignas(8) Page {
        std::array<std::byte, page_size> bytes{};
        Permissions permissions{};
    };
    std::map<std::uint32_t, std::unique_ptr<Page>> pages_;
    std::uint64_t generation_{};
};
}
