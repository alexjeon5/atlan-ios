// SPDX-License-Identifier: GPL-2.0-or-later
// Host implementations of image code that is linked into guest images: the PNG row filters (PNG specification,
// called through libpng's png_read_filter_row) and ETC1 block decoding (Khronos ETC1 specification, called
// through the PowerVR tools' PVRTDecompressETC).
#include "atlan/native_functions.hpp"
#include <algorithm>
#include <array>
#include <cstdlib>

namespace atlan {
namespace {
using Words = std::vector<std::uint32_t>;
using Error = std::optional<std::string>;

[[nodiscard]] bool load32(Memory& memory, std::uint32_t at, std::uint32_t& out) {
    std::array<std::byte, 4> bytes{};
    if (!memory.read(at, bytes)) return false;
    out = 0;
    for (unsigned i = 0; i < 4; ++i) out |= std::to_integer<std::uint32_t>(bytes[i]) << (8 * i);
    return true;
}
[[nodiscard]] bool store32(Memory& memory, std::uint32_t at, std::uint32_t value) {
    std::array<std::byte, 4> bytes{};
    for (unsigned i = 0; i < 4; ++i) bytes[i] = std::byte((value >> (8 * i)) & 255U);
    return memory.write(at, bytes);
}
[[nodiscard]] bool load(Memory& memory, std::uint32_t at, std::vector<std::uint8_t>& out) {
    return memory.read(at, std::as_writable_bytes(std::span(out)));
}
[[nodiscard]] bool store(Memory& memory, std::uint32_t at, const std::vector<std::uint8_t>& data) {
    return memory.write(at, std::as_bytes(std::span(data)));
}
struct Random {
    std::uint32_t state{2463534242U};
    std::uint32_t next() noexcept { state = state * 1664525U + 1013904223U; return state >> 8; }
};

// --- void png_read_filter_row(png_ptr, row_info, row, prev_row, filter): undoes one row's filter in place.
// row_info is libpng 1.2's png_row_info: rowbytes at 4, pixel_depth at 11.
constexpr std::uint32_t max_row_bytes = 1U << 22;
NativeSet png_filter_set() {
    NativeSet set;
    set.name = "png_read_filter_row";
    ImportService service = [](ImportCall& call) -> Error {
        Memory& memory = call.runtime().memory;
        std::uint32_t filter = 0, row_bytes = 0, depth_word = 0;
        if (!call.read_arg(4, filter)) return "png_read_filter_row: unreadable argument";
        if (filter > 4) { call.decline(); return std::nullopt; }  // the guest's own code reports it
        if (filter == 0) return std::nullopt;
        if (!load32(memory, call.arg(1) + 4, row_bytes) || !load32(memory, call.arg(1) + 8, depth_word))
            return "png_read_filter_row: unreadable row information";
        const std::uint32_t bpp = ((depth_word >> 24) + 7) >> 3;
        if (bpp == 0 || row_bytes > max_row_bytes) { call.decline(); return std::nullopt; }
        std::vector<std::uint8_t> row(row_bytes), prior(filter == 1 ? 0 : row_bytes);
        if (!load(memory, call.arg(2), row) || !load(memory, call.arg(3), prior)) return "png_read_filter_row: unreadable row";
        const std::size_t n = row.size();
        switch (filter) {
        case 1:
            for (std::size_t i = bpp; i < n; ++i) row[i] = static_cast<std::uint8_t>(row[i] + row[i - bpp]);
            break;
        case 2:
            for (std::size_t i = 0; i < n; ++i) row[i] = static_cast<std::uint8_t>(row[i] + prior[i]);
            break;
        case 3:
            for (std::size_t i = 0; i < n && i < bpp; ++i) row[i] = static_cast<std::uint8_t>(row[i] + (prior[i] >> 1));
            for (std::size_t i = bpp; i < n; ++i) row[i] = static_cast<std::uint8_t>(row[i] + ((row[i - bpp] + prior[i]) >> 1));
            break;
        default:
            for (std::size_t i = 0; i < n && i < bpp; ++i) row[i] = static_cast<std::uint8_t>(row[i] + prior[i]);
            for (std::size_t i = bpp; i < n; ++i) {
                const int a = row[i - bpp], b = prior[i], c = prior[i - bpp];
                const int pa = std::abs(b - c), pb = std::abs(a - c), pc = std::abs(a + b - 2 * c);
                row[i] = static_cast<std::uint8_t>(row[i] + (pa <= pb && pa <= pc ? a : pb <= pc ? b : c));
            }
        }
        if (!store(memory, call.arg(2), row)) return "png_read_filter_row: unwritable row";
        return std::nullopt;
    };
    set.members.push_back({"png_read_filter_row", {"png_read_filter_row"}, std::move(service), true});
    set.verify = [](NativeProbe& probe, const NativeSet::Call& guest, const NativeSet::Call& native) {
        Memory& memory = probe.memory();
        const std::uint32_t png = probe.scratch(), info = probe.scratch() + 0x1000, row_at = probe.scratch() + 0x2ffd,
                            prior_at = probe.scratch() + 0x6001;
        const std::vector<std::uint8_t> zeros(0x1000);
        Random random;
        for (const std::uint32_t depth : {1U, 4U, 8U, 16U, 24U, 32U, 48U, 64U})
            for (std::uint32_t filter = 0; filter <= 4; ++filter)
                for (const std::uint32_t pixels : {1U, 5U, 96U, 500U}) {
                    const std::uint32_t row_bytes = pixels * ((depth + 7) / 8);  // whole pixels, as libpng passes
                    std::vector<std::uint8_t> row(row_bytes), prior(row_bytes), results[2];
                    for (auto& byte : row) byte = static_cast<std::uint8_t>(random.next());
                    for (auto& byte : prior) byte = static_cast<std::uint8_t>(random.next());
                    const std::uint32_t channels = depth >= 24 ? depth / (depth % 16 == 0 && depth > 32 ? 16 : 8) : 1;
                    for (int side = 0; side < 2; ++side) {
                        if (!store(memory, png, zeros) || !store(memory, row_at, row) || !store(memory, prior_at, prior) ||
                            !store32(memory, info, row_bytes * 8 / depth) || !store32(memory, info + 4, row_bytes) ||
                            !store32(memory, info + 8, 2U | 8U << 8 | channels << 16 | depth << 24)) return false;
                        const Words arguments{png, info, row_at, prior_at, filter};
                        if (!(side == 0 ? guest : native)("png_read_filter_row", arguments)) return false;
                        results[side].resize(row_bytes);
                        if (!load(memory, row_at, results[side])) return false;
                    }
                    if (results[0] != results[1]) return false;
                }
        return true;
    };
    return set;
}

// --- int PVRTDecompressETC(const void* source, const unsigned& width, const unsigned& height, void* dest, const int& mode)
// Decodes ETC1 blocks to 32-bit pixels and returns the number of source bytes. `blue_first` picks the byte
// order of a pixel; which one a build uses shows in the verification.
constexpr std::array<std::array<int, 2>, 8> etc_modifiers{{{2, 8}, {5, 17}, {9, 29}, {13, 42}, {18, 60}, {24, 80}, {33, 106}, {47, 183}}};
void decode_etc1(const std::uint8_t* block, std::uint8_t* out, std::size_t stride, bool blue_first) noexcept {
    int base[2][3];
    const bool differential = (block[3] & 2) != 0, flipped = (block[3] & 1) != 0;
    for (int c = 0; c < 3; ++c) {
        if (differential) {
            const int first = block[c] >> 3, delta = (block[c] & 7) >= 4 ? (block[c] & 7) - 8 : block[c] & 7;
            const int second = (first + delta) & 31;
            base[0][c] = first << 3 | first >> 2;
            base[1][c] = second << 3 | second >> 2;
        } else {
            base[0][c] = (block[c] >> 4) * 17;
            base[1][c] = (block[c] & 15) * 17;
        }
    }
    const int tables[2] = {block[3] >> 5, (block[3] >> 2) & 7};
    const std::uint32_t bits = std::uint32_t{block[4]} << 24 | std::uint32_t{block[5]} << 16 | std::uint32_t{block[6]} << 8 | block[7];
    for (int x = 0; x < 4; ++x)
        for (int y = 0; y < 4; ++y) {
            const int index = x * 4 + y, half = (flipped ? y : x) >= 2 ? 1 : 0;
            const int size = etc_modifiers[static_cast<std::size_t>(tables[half])][(bits >> index) & 1U];
            const int modifier = (bits >> (16 + index)) & 1U ? -size : size;
            std::uint8_t* pixel = out + static_cast<std::size_t>(y) * stride + static_cast<std::size_t>(x) * 4;
            for (int c = 0; c < 3; ++c) {
                const int value = base[half][c] + modifier;
                pixel[blue_first ? 2 - c : c] = static_cast<std::uint8_t>(value < 0 ? 0 : value > 255 ? 255 : value);
            }
            pixel[3] = 255;
        }
}
NativeSet etc_set(bool blue_first) {
    NativeSet set;
    set.name = blue_first ? "PVRTDecompressETC (BGRA)" : "PVRTDecompressETC (RGBA)";
    ImportService service = [blue_first](ImportCall& call) -> Error {
        Memory& memory = call.runtime().memory;
        std::uint32_t width = 0, height = 0, mode_at = 0, mode = 0;
        if (!load32(memory, call.arg(1), width) || !load32(memory, call.arg(2), height) || !call.read_arg(4, mode_at) ||
            !load32(memory, mode_at, mode)) return "PVRTDecompressETC: unreadable argument";
        // A side shorter than one block (the small levels of a mipmap chain) is decoded as a whole block and
        // cropped. Other modes take paths that were not compared; the guest's own code handles them.
        const std::uint32_t block_width = std::max<std::uint32_t>(width, 4), block_height = std::max<std::uint32_t>(height, 4);
        if (mode != 0 || width == 0 || height == 0 || block_width % 4 != 0 || block_height % 4 != 0 || width > 8192 || height > 8192) {
            call.decline();
            return std::nullopt;
        }
        std::vector<std::uint8_t> source(std::size_t{block_width} * block_height / 2), pixels(std::size_t{block_width} * block_height * 4);
        if (!load(memory, call.arg(0), source)) return "PVRTDecompressETC: unreadable source";
        const std::uint8_t* block = source.data();
        for (std::uint32_t y = 0; y < block_height; y += 4)
            for (std::uint32_t x = 0; x < block_width; x += 4, block += 8)
                decode_etc1(block, pixels.data() + (std::size_t{y} * block_width + x) * 4, std::size_t{block_width} * 4, blue_first);
        if (block_width != width || block_height != height) {
            std::vector<std::uint8_t> cropped(std::size_t{width} * height * 4);
            for (std::uint32_t y = 0; y < height; ++y)
                std::copy_n(pixels.begin() + static_cast<std::ptrdiff_t>(std::size_t{y} * block_width * 4), std::size_t{width} * 4,
                            cropped.begin() + static_cast<std::ptrdiff_t>(std::size_t{y} * width * 4));
            pixels = std::move(cropped);
        }
        if (!store(memory, call.arg(3), pixels)) return "PVRTDecompressETC: unwritable destination";
        call.set_result(static_cast<std::uint32_t>(source.size()));
        return std::nullopt;
    };
    set.members.push_back({"PVRTDecompressETC", {"_Z17PVRTDecompressETCPKvRKjS2_PvRKi"}, std::move(service), true});
    set.verify = [](NativeProbe& probe, const NativeSet::Call& guest, const NativeSet::Call& native) {
        Memory& memory = probe.memory();
        const std::uint32_t width_at = probe.scratch(), height_at = probe.scratch() + 4, mode_at = probe.scratch() + 8,
                            source_at = probe.scratch() + 0x1000, dest_at = probe.scratch() + 0x8000;
        Random random;
        const std::array<std::array<std::uint32_t, 2>, 8> sizes{{{4, 4}, {32, 8}, {8, 20}, {64, 64}, {2, 2}, {1, 1}, {8, 2}, {2, 16}}};
        for (const auto& size : sizes) {
            std::vector<std::uint8_t> source(std::max<std::uint32_t>(size[0], 4) * std::max<std::uint32_t>(size[1], 4) / 2), results[2];
            for (auto& byte : source) byte = static_cast<std::uint8_t>(random.next());
            for (std::size_t at = 0; at < source.size(); at += 8) {
                if ((at / 8) % 5 == 0) source[at + 3] &= 0xfd;  // some individual-mode blocks
                if ((source[at + 3] & 2) == 0) continue;
                for (int c = 0; c < 3; ++c) {  // keep differential blocks inside the 5-bit range
                    const int first = source[at + c] >> 3, delta = (source[at + c] & 7) >= 4 ? (source[at + c] & 7) - 8 : source[at + c] & 7;
                    if (first + delta < 0 || first + delta > 31) source[at + c] &= 0xf8;
                }
            }
            std::optional<NativeProbe::Result> returned[2];
            for (int side = 0; side < 2; ++side) {
                const std::vector<std::uint8_t> blank(size[0] * size[1] * 4 + 64, std::uint8_t{0x5a});  // and what must stay untouched
                if (!store32(memory, width_at, size[0]) || !store32(memory, height_at, size[1]) || !store32(memory, mode_at, 0) ||
                    !store(memory, source_at, source) || !store(memory, dest_at, blank)) return false;
                const Words arguments{source_at, width_at, height_at, dest_at, mode_at};
                returned[side] = (side == 0 ? guest : native)("PVRTDecompressETC", arguments);
                results[side].resize(blank.size());
                if (!returned[side] || !load(memory, dest_at, results[side])) return false;
            }
            if (returned[0]->r0 != returned[1]->r0 || results[0] != results[1]) return false;
        }
        return true;
    };
    return set;
}
}

std::vector<NativeSet> image_library_sets() {
    std::vector<NativeSet> sets;
    sets.push_back(png_filter_set());
    sets.push_back(etc_set(false));
    sets.push_back(etc_set(true));
    return sets;
}
}
