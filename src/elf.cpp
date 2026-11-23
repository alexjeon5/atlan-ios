// SPDX-License-Identifier: GPL-2.0-or-later
#include "atlan/elf.hpp"
#include <algorithm>
#include <array>
#include <bit>
#include <map>
#include <set>
#include <stdexcept>
#include <unordered_map>

namespace atlan::elf {
namespace {
constexpr std::uint64_t address_limit = std::uint64_t{1} << 32;
constexpr std::uint32_t trap_base = 0xf0000000;
constexpr std::uint32_t trap_bytes = 0x01000000;
[[noreturn]] void fail(const std::string& message) { throw std::runtime_error("ELF: " + message); }
struct Bytes {
    std::span<const std::byte> data;
    void range(std::uint64_t offset, std::uint64_t size) const {
        if (offset > data.size() || size > data.size() - offset) fail("truncated file");
    }
    std::uint8_t u8(std::uint64_t offset) const {
        range(offset, 1); return std::to_integer<std::uint8_t>(data[offset]);
    }
    std::uint16_t u16(std::uint64_t offset) const {
        return std::uint16_t{u8(offset)} | std::uint16_t{u8(offset + 1)} << 8;
    }
    std::uint32_t u32(std::uint64_t offset) const {
        return std::uint32_t{u16(offset)} | std::uint32_t{u16(offset + 2)} << 16;
    }
};
struct Segment {
    std::uint32_t type{}, offset{}, address{}, file_size{}, memory_size{}, flags{}, alignment{};
};
struct RawSymbol {
    Symbol symbol;
    std::uint16_t section{};
    std::uint8_t visibility{};
    std::uint32_t value{};
};
bool contains(const Segment& s, std::uint32_t at, std::uint64_t size, bool file_backed) {
    const auto length = file_backed ? s.file_size : s.memory_size;
    return at >= s.address && std::uint64_t{at} - s.address <= length &&
           size <= length - (std::uint64_t{at} - s.address);
}
std::uint32_t biased(std::uint32_t value, std::uint32_t bias) {
    const auto result = std::uint64_t{value} + bias;
    if (result >= address_limit) fail("load address exceeds ARM32");
    return static_cast<std::uint32_t>(result);
}
std::uint32_t memory_word(Memory& memory, std::uint32_t address) {
    std::array<std::byte, 4> bytes{};
    if (!memory.read(address, bytes)) fail("unmapped word");
    return Bytes{bytes}.u32(0);
}
void store_word(Memory& memory, std::uint32_t address, std::uint32_t value) {
    std::array<std::byte, 4> bytes{};
    for (unsigned i = 0; i < 4; ++i) bytes[i] = std::byte((value >> (i * 8)) & 255);
    if (!memory.write(address, bytes)) fail("unwritable relocation target");
}
}

const Symbol* Image::find(std::string_view name) const noexcept {
    const auto found = std::find_if(exports.begin(), exports.end(),
        [&](const Symbol& symbol) { return symbol.name == name; });
    return found == exports.end() ? nullptr : &*found;
}
const Import* Image::import_at(std::uint32_t address) const noexcept {
    if (address < trap_base || address >= std::uint64_t{trap_base} + trap_bytes) return nullptr;
    const auto found = std::find_if(trapped_imports.begin(), trapped_imports.end(),
        [&](const Import& item) { return address >= item.address && address - item.address < 16; });
    return found == trapped_imports.end() ? nullptr : &*found;
}

Image load(std::span<const std::byte> file, const Options& options) {
    const Bytes bytes{file};
    bytes.range(0, 52);
    if (bytes.u32(0) != 0x464c457f || bytes.u8(4) != 1 || bytes.u8(5) != 1 ||
        bytes.u8(6) != 1 || (bytes.u8(7) != 0 && bytes.u8(7) != 3) ||
        bytes.u16(16) != 3 || bytes.u16(18) != 40 || bytes.u32(20) != 1 ||
        bytes.u16(40) != 52 || bytes.u16(42) != 32 ||
        (bytes.u32(36) & 0xff000000U) != 0x05000000U)
        fail("requires little-endian ELF32 ARM EABI5 shared object");
    if (options.load_bias % Memory::page_size != 0) fail("unaligned load bias");
    const auto phoff = bytes.u32(28);
    const auto phnum = bytes.u16(44);
    if (phnum == 0 || phnum > 4096) fail("unsupported program-header count");
    bytes.range(phoff, std::uint64_t{phnum} * 32);
    std::vector<Segment> loads;
    std::optional<Segment> dynamic;
    std::vector<Segment> relro;
    for (unsigned i = 0; i < phnum; ++i) {
        const auto at = std::uint64_t{phoff} + i * 32;
        const Segment s{bytes.u32(at), bytes.u32(at + 4), bytes.u32(at + 8),
                        bytes.u32(at + 16), bytes.u32(at + 20), bytes.u32(at + 24), bytes.u32(at + 28)};
        if (s.type == 7) fail("TLS segments are unsupported");
        if (s.type == 1) {
            if (s.file_size > s.memory_size || std::uint64_t{s.address} + s.memory_size > address_limit ||
                (s.alignment > 1 && (!std::has_single_bit(s.alignment) ||
                 s.address % s.alignment != s.offset % s.alignment)) ||
                s.address % Memory::page_size != s.offset % Memory::page_size ||
                (s.alignment > 1 && options.load_bias % s.alignment != 0) ||
                (s.flags & ~7U) != 0 || (s.flags & 4U) == 0 || (s.flags & 3U) == 3U)
                fail("invalid or unsupported load segment");
            bytes.range(s.offset, s.file_size);
            if (s.memory_size != 0) loads.push_back(s);
        } else if (s.type == 2) {
            if (dynamic) fail("multiple dynamic segments");
            dynamic = s;
        } else if (s.type == 0x6474e552) {
            relro.push_back(s);
        } else if (s.type == 0x6474e551 && (s.flags & 1U) != 0) {
            fail("executable stack is unsupported");
        }
    }
    if (loads.empty() || !dynamic) fail("missing load or dynamic segment");
    std::sort(loads.begin(), loads.end(), [](const Segment& a, const Segment& b) { return a.address < b.address; });
    for (std::size_t i = 1; i < loads.size(); ++i)
        if (std::uint64_t{loads[i-1].address} + loads[i-1].memory_size > loads[i].address)
            fail("overlapping segment contents");
    const auto file_offset = [&](std::uint32_t address, std::uint64_t size) -> std::uint64_t {
        for (const auto& s : loads)
            if (contains(s, address, size, true)) return std::uint64_t{s.offset} + address - s.address;
        fail("dynamic table points outside file-backed load segments");
    };
    const auto in_memory = [&](std::uint32_t address, std::uint64_t size, unsigned required_flags = 0U) {
        return std::any_of(loads.begin(), loads.end(), [&](const Segment& s) {
            return contains(s, address, size, false) && (s.flags & required_flags) == required_flags;
        });
    };

    // No section headers are required: all metadata comes from PT_DYNAMIC.
    if (dynamic->file_size == 0 || dynamic->file_size % 8 != 0) fail("invalid dynamic size");
    const auto dynamic_offset = file_offset(dynamic->address, dynamic->file_size);
    if (dynamic_offset != dynamic->offset) fail("inconsistent dynamic file offset");
    std::unordered_map<std::uint32_t, std::uint32_t> tags;
    std::vector<std::uint32_t> needed;
    bool terminated = false;
    for (std::uint64_t i = 0; i < dynamic->file_size; i += 8) {
        const auto tag = bytes.u32(dynamic_offset + i), value = bytes.u32(dynamic_offset + i + 4);
        if (tag == 0) { terminated = true; break; }
        if (tag == 1) needed.push_back(value);
        else if (!tags.emplace(tag, value).second) fail("duplicate dynamic tag");
    }
    if (!terminated) fail("unterminated dynamic table");
    for (const auto tag : {7U, 8U, 9U, 22U, 0x6000000fU, 0x60000010U, 0x60000011U,
                           0x60000012U, 0x6ffffff0U, 0x6ffffffeU, 0x6ffffffcU})
        if (tags.contains(tag)) fail("unsupported relocation or versioning feature");
    if ((tags.contains(30) && (tags.at(30) & (4U | 16U)) != 0)) fail("text relocations or static TLS unsupported");
    const auto required = [&](std::uint32_t tag) {
        if (!tags.contains(tag)) fail("missing dynamic tag " + std::to_string(tag));
        return tags.at(tag);
    };
    const auto string_address = required(5), string_size = required(10);
    if (string_size == 0 || string_size > 16 * 1024 * 1024) fail("invalid string table size");
    const auto strings_offset = file_offset(string_address, string_size);
    const auto string_at = [&](std::uint32_t index) {
        if (index >= string_size) fail("string offset out of range");
        auto end = index;
        while (end < string_size && bytes.u8(strings_offset + end) != 0) ++end;
        if (end == string_size) fail("unterminated string");
        return std::string(reinterpret_cast<const char*>(file.data() + strings_offset + index), end - index);
    };
    const auto hash_offset = file_offset(required(4), 8);
    const auto buckets = bytes.u32(hash_offset), symbol_count = bytes.u32(hash_offset + 4);
    if (buckets == 0 || symbol_count == 0 || symbol_count > 1024 * 1024) fail("invalid SysV hash table");
    (void)file_offset(required(4), 8 + (std::uint64_t{buckets} + symbol_count) * 4);
    if (required(11) != 16) fail("invalid symbol entry size");
    const auto symbol_offset = file_offset(required(6), std::uint64_t{symbol_count} * 16);
    std::vector<RawSymbol> symbols;
    symbols.reserve(symbol_count);
    Image image;
    image.load_bias = options.load_bias;
    for (const auto index : needed) image.dependencies.push_back(string_at(index));
    for (std::uint32_t i = 0; i < symbol_count; ++i) {
        const auto at = symbol_offset + std::uint64_t{i} * 16;
        RawSymbol s;
        s.symbol.name = string_at(bytes.u32(at));
        s.value = bytes.u32(at + 4);
        s.symbol.size = bytes.u32(at + 8);
        s.symbol.binding = bytes.u8(at + 12) >> 4;
        s.symbol.type = bytes.u8(at + 12) & 15;
        s.visibility = bytes.u8(at + 13) & 3;
        s.section = bytes.u16(at + 14);
        if (s.symbol.type == 6 || s.symbol.type == 10 || s.symbol.binding > 2 ||
            (s.section >= 0xff00 && s.section != 0xfff1)) fail("unsupported symbol kind");
        if (s.section != 0) {
            if (s.section != 0xfff1 && !in_memory(s.value & (s.symbol.type == 2 ? ~1U : ~0U), s.symbol.size))
                fail("defined symbol outside load segments");
            s.symbol.address = s.section == 0xfff1 ? s.value : biased(s.value, options.load_bias);
            if (s.symbol.binding != 0 && (s.visibility == 0 || s.visibility == 3))
                image.exports.push_back(s.symbol);
        }
        symbols.push_back(std::move(s));
    }

    // Stage writable pages, apply relocations, then seal guest permissions.
    std::map<std::uint32_t, unsigned> page_flags;
    for (const auto& s : loads) {
        const auto start = std::uint64_t{biased(s.address, options.load_bias)};
        const auto end = start + s.memory_size;
        if (end > address_limit || (start < std::uint64_t{trap_base} + trap_bytes && end > trap_base))
            fail("segment overlaps ARM32 limit or diagnostic import reservation");
        for (auto page = start & ~std::uint64_t{4095}; page < end; page += Memory::page_size) {
            const auto flags = page_flags[static_cast<std::uint32_t>(page)] |= s.flags;
            if ((flags & 3U) == 3U) fail("shared segment page would be writable and executable");
            if (page_flags.size() > options.max_mapped_bytes / Memory::page_size) fail("mapping budget exceeded");
        }
    }
    image.mapped_bytes = page_flags.size() * std::uint64_t{Memory::page_size};
    for (const auto& [page, flags] : page_flags) {
        (void)flags;
        image.memory.map(page, Memory::page_size, Permissions::read_write);
    }
    for (const auto& s : loads)
        if (!image.memory.write(biased(s.address, options.load_bias), file.subspan(s.offset, s.file_size)))
            fail("segment copy failed");

    std::vector<std::optional<std::uint32_t>> resolved(symbol_count);
    const auto resolve = [&](std::uint32_t index) -> std::uint32_t {
        if (index >= symbols.size()) fail("relocation symbol index out of range");
        if (resolved[index]) return *resolved[index];
        const auto& symbol = symbols[index];
        std::uint32_t address = 0;
        if (symbol.section != 0) address = symbol.symbol.address;
        else if (index != 0) {
            const auto found = options.resolve ? options.resolve(symbol.symbol.name, symbol.symbol.type) : std::nullopt;
            if (found) address = *found;
            else if (symbol.symbol.binding == 2) address = 0;
            else {
                if (options.unresolved == UnresolvedImports::reject)
                    fail("unresolved import: " + symbol.symbol.name);
                if (image.trapped_imports.size() >= trap_bytes / 16) fail("too many unresolved imports");
                address = trap_base + static_cast<std::uint32_t>(image.trapped_imports.size()) * 16;
                image.trapped_imports.push_back({symbol.symbol.name, address, symbol.symbol.type});
            }
        }
        resolved[index] = address;
        return address;
    };
    std::set<std::uint32_t> relocated;
    const auto relocate = [&](std::uint32_t address, std::uint32_t size) {
        if (size % 8 != 0 || size / 8 > 1024 * 1024) fail("invalid REL table size");
        if (size == 0) return;
        const auto offset = file_offset(address, size);
        for (std::uint64_t i = 0; i < size; i += 8) {
            const auto place = bytes.u32(offset + i), info = bytes.u32(offset + i + 4);
            const auto type = info & 255, symbol_index = info >> 8;
            if (type == 0) continue;
            if (type != 2 && type != 21 && type != 22 && type != 23) fail("unsupported relocation " + std::to_string(type));
            if (place % 4 != 0 || !in_memory(place, 4, 2) || !relocated.insert(place).second)
                fail("invalid or duplicate relocation target");
            const auto target = biased(place, options.load_bias);
            const auto addend = memory_word(image.memory, target);
            std::uint32_t value;
            if (type == 23) {
                if (symbol_index != 0) fail("symbol-bearing RELATIVE unsupported");
                value = options.load_bias + addend;
            } else {
                const auto address_value = resolve(symbol_index);
                if (type == 2) {
                    const auto thumb = symbols[symbol_index].symbol.type == 2 ? address_value & 1U : 0U;
                    value = ((address_value & ~thumb) + addend) | thumb;
                } else {
                    // AAELF32 dynamic semantics: GLOB_DAT/JUMP_SLOT replace
                    // the word with S, including its Thumb bit, not S+A.
                    value = address_value;
                }
            }
            store_word(image.memory, target, value);
            ++image.relocation_count;
        }
    };
    if (tags.contains(17) || tags.contains(18)) {
        if (required(19) != 8) fail("invalid REL entry size");
        relocate(required(17), required(18));
    }
    if (tags.contains(23) || tags.contains(2)) {
        if (required(20) != 17) fail("PLT requires REL relocations");
        relocate(required(23), required(2));
    }
    const auto initializer = [&](std::uint32_t address) {
        if (address == 0 || address == UINT32_MAX) return;
        const auto raw = address - options.load_bias;
        if (address < options.load_bias || !in_memory(raw & ~1U, 2, 1)) fail("initializer outside executable segment");
        image.initializers.push_back(address);
    };
    if (tags.contains(12) && tags.at(12) != 0) initializer(biased(tags.at(12), options.load_bias));
    if (tags.contains(25) || tags.contains(27)) {
        const auto size = required(27), address = required(25);
        if (size % 4 != 0 || size / 4 > 1024 * 1024 || !in_memory(address, size)) fail("invalid init array");
        for (std::uint64_t i = 0; i < size; i += 4)
            initializer(memory_word(image.memory, biased(static_cast<std::uint32_t>(address + i), options.load_bias)));
    }
    for (const auto& [page, flags] : page_flags)
        image.memory.protect(page, Memory::page_size, (flags & 1U) ? Permissions::read_execute :
                             (flags & 2U) ? Permissions::read_write : Permissions::read);
    for (const auto& region : relro) {
        if (!in_memory(region.address, region.memory_size, 2)) fail("invalid RELRO segment");
        // ELF RELRO rounds its end down: a partial final page may contain
        // mutable data and must keep its ordinary permissions.
        const auto start = std::uint64_t{biased(region.address, options.load_bias)} & ~std::uint64_t{4095};
        const auto end = (std::uint64_t{biased(region.address, options.load_bias)} + region.memory_size) & ~std::uint64_t{4095};
        if (end > start) image.memory.protect(static_cast<std::uint32_t>(start), end - start, Permissions::read);
    }
    return image;
}
}
