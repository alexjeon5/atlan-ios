// SPDX-License-Identifier: GPL-2.0-or-later
#include "adapters/atlan/poi_lookup.hpp"
#include <array>
#include <cstdint>
#include <string>

namespace atlan::shell::poi {
namespace {
// The map settings object, as these lookups read it.
constexpr std::uint32_t category_count_at = 24, categories_at = 196, symbols_limit_at = 416, symbols_at = 772,
                        map_level_at = 1916, alternate_size_at = 1936, wanted_variant_at = 2164, draw_mode_at = 2172;
// A category entry (24 bytes) and a variant (24 bytes).
constexpr std::uint32_t entry_bytes = 24, variant_bytes = 24, symbol_bytes = 20;

struct Reader {
    Memory& memory;
    bool ok{true};
    std::uint32_t u8(std::uint32_t at) {
        std::array<std::byte, 1> b{};
        if (!memory.read(at, b)) ok = false;
        return std::to_integer<std::uint32_t>(b[0]);
    }
    std::uint32_t u16(std::uint32_t at) {
        std::array<std::byte, 2> b{};
        if (!memory.read(at, b)) ok = false;
        return std::to_integer<std::uint32_t>(b[0]) | std::to_integer<std::uint32_t>(b[1]) << 8;
    }
    std::uint32_t u32(std::uint32_t at) {
        std::array<std::byte, 4> b{};
        if (!memory.read(at, b)) ok = false;
        std::uint32_t v = 0;
        for (unsigned i = 0; i < 4; ++i) v |= std::to_integer<std::uint32_t>(b[i]) << (8 * i);
        return v;
    }
    std::array<std::uint8_t, 24> record(std::uint32_t at) {
        std::array<std::uint8_t, 24> r{};
        if (!memory.read(at, std::as_writable_bytes(std::span(r)))) ok = false;
        return r;
    }
};
[[nodiscard]] std::uint32_t half(const std::array<std::uint8_t, 24>& r, unsigned at) { return r[at] | std::uint32_t{r[at + 1]} << 8; }
[[nodiscard]] bool writable(Memory& memory, std::uint32_t at, std::uint32_t n) {
    return memory.translate(at, MemoryAccess::write) != nullptr && memory.translate(std::uint64_t{at} + n - 1, MemoryAccess::write) != nullptr;
}
[[nodiscard]] bool store(Memory& memory, std::uint32_t at, std::uint32_t value) {
    std::array<std::byte, 4> b{};
    for (unsigned i = 0; i < 4; ++i) b[i] = std::byte((value >> (8 * i)) & 255U);
    return memory.write(at, b);
}

// The variant of a category for the wanted id: the first with that id, else the first default (id 1), else the
// first. nullopt with `none`: the category has no variants.
std::optional<std::array<std::uint8_t, 24>> pick(Reader& in, std::uint32_t list, std::uint32_t variants, std::uint32_t wanted) {
    for (std::uint32_t i = 0; i < variants; ++i) {
        const auto v = in.record(list + i * variant_bytes);
        if (!in.ok) return std::nullopt;
        if (v[0] == wanted) return v;
    }
    for (std::uint32_t i = 0; i < variants; ++i) {
        const auto v = in.record(list + i * variant_bytes);
        if (!in.ok) return std::nullopt;
        if (v[0] == 1) return v;
    }
    return in.record(list);
}

// bool style(settings, category, -, -, record& out): fills the 20-byte style record. false: no such category or
// no variants. Declines (false return) only when guest memory faults; `result` holds the guest's return value.
bool style(Memory& memory, std::uint32_t self, std::uint32_t category, std::uint32_t out, std::uint32_t& result) {
    Reader in{memory};
    const std::uint32_t count = in.u16(self + category_count_at);
    if (!in.ok) return false;
    if (!(static_cast<std::int32_t>(count) > static_cast<std::int32_t>(category))) { result = 0; return true; }
    const std::uint32_t entry = in.u32(self + categories_at) + entry_bytes * category;
    std::uint32_t wanted = in.u32(self + wanted_variant_at);
    const std::uint32_t b3 = in.u8(entry + 3), variants = in.u8(entry + 6), b7 = in.u8(entry + 7), b8 = in.u8(entry + 8),
                        b9 = in.u8(entry + 9), b10 = in.u8(entry + 10), word = in.u32(entry + 12), list = in.u32(entry + 20);
    const std::uint32_t mode = in.u32(self + draw_mode_at);
    if (!in.ok) return false;
    if (mode - 1 <= 2) wanted = 4;
    if (mode - 2 <= 1) wanted = b7 == 1 ? 2 : 4;
    if (variants == 0) { result = 0; return true; }
    const auto chosen = pick(in, list, variants, wanted);
    const std::uint32_t alternate = in.u32(self + alternate_size_at);
    if (!chosen || !in.ok || !writable(memory, out, 20)) return false;
    const auto& c = *chosen;
    const std::uint32_t size = alternate != 0 ? half(c, 6) : half(c, 4), other = half(c, 18);
    const std::array<std::uint8_t, 20> record{c[0], c[1], c[2], c[3], static_cast<std::uint8_t>(other), static_cast<std::uint8_t>(other >> 8),
                                              static_cast<std::uint8_t>(size), static_cast<std::uint8_t>(size >> 8),
                                              static_cast<std::uint8_t>(word), static_cast<std::uint8_t>(word >> 8),
                                              static_cast<std::uint8_t>(word >> 16), static_cast<std::uint8_t>(word >> 24),
                                              c[13], c[12], c[14], static_cast<std::uint8_t>(b7), static_cast<std::uint8_t>(b8),
                                              static_cast<std::uint8_t>(b9), static_cast<std::uint8_t>(b10), static_cast<std::uint8_t>(b3)};
    if (!memory.write(out, std::as_bytes(std::span(record)))) return false;
    result = 1;
    return true;
}

// bool symbol(settings, category, -, int& a, int& b, int& c, int& d, int& e): the icon's symbol data. The variant
// must also cover the current map level; the default variant covers every level.
bool symbol(Memory& memory, std::uint32_t self, std::uint32_t category, std::array<std::uint32_t, 5> outs, std::uint32_t& result) {
    Reader in{memory};
    const std::uint32_t count = in.u16(self + category_count_at);
    if (!in.ok) return false;
    if (!(static_cast<std::int32_t>(count) > static_cast<std::int32_t>(category))) { result = 0; return true; }
    const auto level = static_cast<std::int32_t>(in.u32(self + map_level_at));
    const std::uint32_t entry = in.u32(self + categories_at) + entry_bytes * category;
    const std::uint32_t variants = in.u8(entry + 6), list = in.u32(entry + 20);
    if (!in.ok) return false;
    if (variants == 0) { result = 0; return true; }
    std::optional<std::array<std::uint8_t, 24>> chosen;
    for (std::uint32_t i = 0; i < variants && !chosen; ++i) {
        const auto v = in.record(list + i * variant_bytes);
        const std::uint32_t wanted = in.u32(self + wanted_variant_at);
        if (!in.ok) return false;
        if (wanted == v[0] && level >= v[1] && level <= v[2]) chosen = v;
    }
    for (std::uint32_t i = 0; i < variants && !chosen; ++i) {
        const auto v = in.record(list + i * variant_bytes);
        if (!in.ok) return false;
        if (v[0] == 1) chosen = v;
    }
    if (!chosen) { result = 0; return true; }
    const auto& c = *chosen;
    const std::uint32_t index = half(c, 18);
    if (index == 0) { result = 0; return true; }
    const std::uint32_t limit = in.u16(self + symbols_limit_at);
    if (!in.ok) return false;
    if (limit < index) { result = 0; return true; }
    const std::uint32_t table = in.u32(self + symbols_at);
    const std::uint32_t at = c[13] != 4 ? table + symbol_bytes * index : table + symbol_bytes;
    if (!in.ok) return false;
    if (at == 0) { result = 0; return true; }
    const std::uint32_t first = in.u16(at + 8), second = in.u16(at + 10);
    if (!in.ok) return false;
    for (const std::uint32_t out : outs)
        if (!writable(memory, out, 4)) return false;
    const std::array<std::uint32_t, 5> values{first, second, half(c, 8), half(c, 10), half(c, 16)};
    for (unsigned i = 0; i < 5; ++i)
        if (!store(memory, outs[i], values[i])) return false;
    result = 1;
    return true;
}

bool stack_words(Memory& memory, std::uint32_t sp, std::span<std::uint32_t> out) {
    Reader in{memory};
    for (std::size_t i = 0; i < out.size(); ++i) out[i] = in.u32(sp + 4 * static_cast<std::uint32_t>(i));
    return in.ok;
}

bool style_inline(CpuState& cpu, Memory& memory) noexcept {
    std::array<std::uint32_t, 1> out{};
    std::uint32_t result = 0;
    if (!stack_words(memory, static_cast<std::uint32_t>(cpu.registers[13]), out) ||
        !style(memory, static_cast<std::uint32_t>(cpu.registers[0]), static_cast<std::uint32_t>(cpu.registers[1]), out[0], result))
        return false;
    cpu.registers[0] = result;
    return true;
}
bool symbol_inline(CpuState& cpu, Memory& memory) noexcept {
    std::array<std::uint32_t, 4> stack{};
    std::uint32_t result = 0;
    if (!stack_words(memory, static_cast<std::uint32_t>(cpu.registers[13]), stack) ||
        !symbol(memory, static_cast<std::uint32_t>(cpu.registers[0]), static_cast<std::uint32_t>(cpu.registers[1]),
                {static_cast<std::uint32_t>(cpu.registers[3]), stack[0], stack[1], stack[2], stack[3]}, result))
        return false;
    cpu.registers[0] = result;
    return true;
}

// The guest's own code for anything the host cannot serve (a fault): the trap declines and the guest faults itself.
ImportService serving(InlineRun run) {
    return [run](ImportCall& call) -> std::optional<std::string> {
        if (!run(call.runtime().cpu, call.runtime().memory)) call.decline();
        return std::nullopt;
    };
}

struct Random {
    std::uint32_t state{0x9e3779b9U};
    std::uint32_t next() noexcept { state = state * 1664525U + 1013904223U; return state >> 8; }
    std::uint32_t below(std::uint32_t n) noexcept { return next() % n; }
};

// Generated settings in scratch memory: a settings object, a category table (with one entry before it, for a
// negative category), variant lists and a symbol table. Every case runs the guest and the host on the same bytes.
bool verify(NativeProbe& probe, const NativeSet::Call& guest, const NativeSet::Call& native) {
    Memory& memory = probe.memory();
    const std::uint32_t base = probe.scratch(), self = base, entries = base + 0x1000 + entry_bytes, lists = base + 0x2000,
                        symbols = base + 0x4000, outs = base + 0x6000, arena = 0x7000;
    Random random;
    std::vector<std::uint8_t> bytes(arena);
    const auto put32 = [&](std::uint32_t at, std::uint32_t v) { for (unsigned i = 0; i < 4; ++i) bytes[at - base + i] = static_cast<std::uint8_t>(v >> (8 * i)); };
    const auto put16 = [&](std::uint32_t at, std::uint32_t v) { bytes[at - base] = static_cast<std::uint8_t>(v); bytes[at - base + 1] = static_cast<std::uint8_t>(v >> 8); };
    for (unsigned round = 0; round < 400; ++round) {
        for (auto& b : bytes) b = static_cast<std::uint8_t>(random.next());
        const std::uint32_t count = random.below(6);
        put16(self + category_count_at, count);
        put32(self + categories_at, entries);
        put16(self + symbols_limit_at, random.below(12));
        put32(self + symbols_at, symbols);
        put32(self + map_level_at, random.below(12) - 2);
        put32(self + alternate_size_at, random.below(3) == 0 ? 0 : random.next());
        put32(self + wanted_variant_at, random.below(6));
        put32(self + draw_mode_at, random.below(6));
        for (std::uint32_t e = 0; e < 7; ++e) {  // entries -1 .. 5
            const std::uint32_t entry = entries + entry_bytes * e - entry_bytes;
            bytes[entry - base + 6] = static_cast<std::uint8_t>(random.below(5));
            bytes[entry - base + 7] = static_cast<std::uint8_t>(random.below(3));
            put32(entry + 20, lists + 0x100 * e);
            for (std::uint32_t v = 0; v < 4; ++v) {
                const std::uint32_t variant = lists + 0x100 * e + variant_bytes * v;
                bytes[variant - base] = static_cast<std::uint8_t>(random.below(6));
                bytes[variant - base + 1] = static_cast<std::uint8_t>(random.below(10));
                bytes[variant - base + 2] = static_cast<std::uint8_t>(random.below(12));
                bytes[variant - base + 13] = static_cast<std::uint8_t>(random.below(3) == 0 ? 4 : random.next());
                put16(variant + 18, random.below(5) == 0 ? 0 : random.below(14));
            }
        }
        for (const std::int32_t category : {-1, 0, 1, 2, 3, 4, 5}) {
            for (const bool is_style : {true, false}) {
                std::optional<NativeProbe::Result> returned[2];
                std::vector<std::uint8_t> after[2];
                for (int side = 0; side < 2; ++side) {
                    if (!memory.write(base, std::as_bytes(std::span(bytes)))) return false;
                    const std::vector<std::uint32_t> arguments =
                        is_style ? std::vector<std::uint32_t>{self, static_cast<std::uint32_t>(category), random.next(), random.next(), outs}
                                 : std::vector<std::uint32_t>{self, static_cast<std::uint32_t>(category), random.next(), outs, outs + 4, outs + 8, outs + 12, outs + 16};
                    returned[side] = (side == 0 ? guest : native)(is_style ? "poi_style" : "poi_symbol", arguments);
                    after[side].resize(arena);
                    if (!returned[side] || !memory.read(base, std::as_writable_bytes(std::span(after[side])))) return false;
                }
                // The guest's stack scratch lies outside these bytes; only results and the generated tables are compared.
                if ((returned[0]->r0 & 0xff) != (returned[1]->r0 & 0xff) || after[0] != after[1]) return false;
            }
        }
    }
    return true;
}
}

std::vector<NativeSet> poi_lookup_sets() {
    NativeSet set;
    set.name = "POI style lookups";
    set.members.push_back({"poi_style", {}, serving(style_inline), true, style_inline});
    set.members.push_back({"poi_symbol", {}, serving(symbol_inline), true, symbol_inline});
    set.verify = verify;
    return {std::move(set)};
}
}
