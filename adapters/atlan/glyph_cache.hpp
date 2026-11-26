// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include "atlan/native_functions.hpp"
#include <cstdint>
#include <optional>

// A larger font glyph cache.
//
// The app keeps rendered glyph bitmaps in a cache whose memory pool it creates with a fixed size of a few KB. A screen
// with more distinct glyphs than fit (Korean text: the location terms, lists, menus) evicts and re-renders every
// glyph on every frame, hinting included, which took over 90% of such a frame. The instruction that loads the pool
// size (an A32 MOV of an immediate) is replaced by one that loads `glyph_pool_bytes` instead; the app's own code then
// creates and uses the larger pool as before.
//
// The instruction is located through the embedder's signature file under the member name "glyph_pool_size"; one
// that is not found, or is not such a MOV of a smaller size, is left alone.
namespace atlan::shell::glyph_cache {
inline constexpr std::uint32_t glyph_pool_bytes = 1U << 20;
// The value an always-executed A32 `MOV Rd, #imm` (S clear, Rd not the PC) loads, and its Rd; nullopt for any other
// encoding.
struct MoveImmediate {
    unsigned rd{};
    std::uint32_t value{};
};
[[nodiscard]] std::optional<MoveImmediate> decode_move_immediate(std::uint32_t encoding) noexcept;
[[nodiscard]] NativeSet glyph_cache_set();
}
