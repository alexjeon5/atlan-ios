// SPDX-License-Identifier: GPL-2.0-or-later
#include "atlan/data_symbols.hpp"
#include <stdexcept>
#include <vector>

namespace atlan {
void BionicData::map(Memory& memory) {
    if (memory.translate(base, MemoryAccess::read) != nullptr) throw std::invalid_argument("Bionic data page is already mapped");
    std::vector<std::byte> page(Memory::page_size);
    constexpr std::uint32_t table_offset = 0x100;
    for (unsigned i = 0; i < 4; ++i) page[i] = std::byte(((base + table_offset) >> (8 * i)) & 255);  // the pointer variable
    const auto put16 = [&](std::size_t index, std::uint16_t value) {
        page[table_offset + 2 * index] = std::byte(value & 255);
        page[table_offset + 2 * index + 1] = std::byte(value >> 8);
    };
    put16(0, 0xffff);
    for (unsigned c = 0; c < 256; ++c) put16(1 + c, static_cast<std::uint16_t>(c >= 'a' && c <= 'z' ? c - 32 : c));
    memory.map(base, Memory::page_size, Permissions::read, page);
}
}
