// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <string>

// SHA-256 (FIPS 180-4), for identifying a user's guest library without keeping or sending it.
namespace atlan::shell {
class Sha256 {
public:
    Sha256() noexcept;
    void update(std::span<const std::byte> data) noexcept;
    // The digest as 64 lowercase hex digits. The object is spent afterwards.
    [[nodiscard]] std::string hex() noexcept;
private:
    void block(const std::uint8_t* data) noexcept;
    std::array<std::uint32_t, 8> state_{};
    std::array<std::uint8_t, 64> buffer_{};
    std::size_t used_{};
    std::uint64_t length_{};
};
[[nodiscard]] std::string sha256_hex(std::span<const std::byte> data);
}
