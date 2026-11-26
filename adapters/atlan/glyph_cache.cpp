// SPDX-License-Identifier: GPL-2.0-or-later
#include "adapters/atlan/glyph_cache.hpp"
#include <bit>

namespace atlan::shell::glyph_cache {
std::optional<MoveImmediate> decode_move_immediate(std::uint32_t encoding) noexcept {
    // cond 1110, 00 I=1 opcode 1101 S=0, Rn 0000.
    if ((encoding & 0xffff0000U) != 0xe3a00000U) return std::nullopt;
    const unsigned rd = encoding >> 12 & 15;
    if (rd == 15) return std::nullopt;
    const unsigned rotate = (encoding >> 8 & 15) * 2;
    return MoveImmediate{rd, std::rotr(encoding & 0xffU, static_cast<int>(rotate))};
}

NativeSet glyph_cache_set() {
    NativeSet out;
    out.name = "glyph cache size";
    NativeSet::Member size;
    size.name = "glyph_pool_size";
    out.members.push_back(std::move(size));
    out.instructions["glyph_pool_size"] = [](std::uint32_t encoding) -> ImportService {
        const auto move = decode_move_immediate(encoding);
        if (!move || move->value >= glyph_pool_bytes) return {};
        return [rd = move->rd](ImportCall& call) -> std::optional<std::string> {
            call.runtime().cpu.registers[rd] = glyph_pool_bytes;
            return std::nullopt;
        };
    };
    return out;
}
}
