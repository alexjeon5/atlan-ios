// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include "atlan/memory.hpp"
#include <functional>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace atlan::elf {
struct Symbol {
    std::string name;
    std::uint32_t address{};
    std::uint32_t size{};
    std::uint8_t type{};
    std::uint8_t binding{};
};
struct Import {
    std::string name;
    std::uint32_t address{};
    std::uint8_t type{};
};
enum class UnresolvedImports { reject, diagnostic_traps };
struct Options {
    std::uint32_t load_bias{0x10000000};
    std::uint64_t max_mapped_bytes{128 * 1024 * 1024};
    UnresolvedImports unresolved{UnresolvedImports::reject};
    // Every implementation address is a guest address, never a host pointer.
    std::function<std::optional<std::uint32_t>(std::string_view, std::uint8_t)> resolve;
};
struct Image {
    Memory memory;
    std::vector<Symbol> exports;
    std::vector<Import> trapped_imports;
    std::vector<std::string> dependencies;
    std::vector<std::uint32_t> initializers;
    std::uint32_t load_bias{};
    std::uint64_t mapped_bytes{};
    std::uint32_t relocation_count{};

    [[nodiscard]] const Symbol* find(std::string_view name) const noexcept;
    [[nodiscard]] const Import* import_at(std::uint32_t address) const noexcept;
};

// ELF32 little-endian ARM EABI5 ET_DYN with SysV hash and REL relocations.
// Constructs a private image; failure never publishes a partially linked one.
// Constructors are reported, never executed automatically.
[[nodiscard]] Image load(std::span<const std::byte> file, const Options& options = {});
}
