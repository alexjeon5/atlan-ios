// SPDX-License-Identifier: GPL-2.0-or-later
// Host implementations of image code that is linked into guest images: the PNG row filters (PNG specification,
// called through libpng's png_read_filter_row), ETC1 block decoding (Khronos ETC1 specification, called
// through the PowerVR tools' PVRTDecompressETC) and the byte source the PowerVR tools' POD model reader reads from.
#include "atlan/native_functions.hpp"
#include <algorithm>
#include <array>
#include <cstdlib>
#include <memory>

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

// --- The byte source of the PowerVR tools' POD model reader (CSource, CSourceStream over a CPVRTResourceFile).
// A model is parsed from memory in many reads of a few bytes, each a virtual call. The members are accepted only when
// their code is exactly the A32 code they are known as (checked at install, branch offsets aside), which fixes the
// object layout: stream {vtable, file, position}, file {.., .., size at 8, data at 12}, Read at vtable slot 2.
namespace pod {
constexpr std::uint32_t file_at = 4, position_at = 8, size_at = 8, data_at = 12, read_slot = 8;
constexpr std::uint32_t max_read = 1U << 24;  // larger reads stay with the guest's code

enum class Outcome { done, decline };
// A word (or a few bytes) that can be written: both ends translate.
[[nodiscard]] bool writable(Memory& memory, std::uint32_t at, std::uint32_t n) {
    return memory.translate(at, MemoryAccess::write) != nullptr && memory.translate(std::uint64_t{at} + n - 1, MemoryAccess::write) != nullptr;
}
// bool CSourceStream::Read(void* to, unsigned n): copies n bytes at the position and advances it, unless position + n
// (32-bit sum) passes the file's size. The position is read again after the copy, as the guest does.
[[nodiscard]] Outcome stream_read(Memory& memory, std::uint32_t self, std::uint32_t to, std::uint32_t n, std::uint32_t& result) {
    std::uint32_t file = 0, position = 0, size = 0, data = 0;
    if (n > max_read || !load32(memory, self + file_at, file) || !load32(memory, self + position_at, position) ||
        !load32(memory, file + size_at, size))
        return Outcome::decline;
    if (position + n > size) { result = 0; return Outcome::done; }
    if (!load32(memory, file + data_at, data) || !writable(memory, self + position_at, 4)) return Outcome::decline;
    if (n != 0 && !memory.move(to, data + position, n)) return Outcome::decline;  // all or nothing
    if (!load32(memory, self + position_at, position) || !store32(memory, self + position_at, position + n)) return Outcome::decline;
    result = 1;
    return Outcome::done;
}
// bool CSourceStream::Skip(unsigned n)
[[nodiscard]] Outcome stream_skip(Memory& memory, std::uint32_t self, std::uint32_t n, std::uint32_t& result) {
    std::uint32_t file = 0, position = 0, size = 0;
    if (!load32(memory, self + file_at, file) || !load32(memory, self + position_at, position) || !load32(memory, file + size_at, size) ||
        !writable(memory, self + position_at, 4))
        return Outcome::decline;
    result = position + n <= size ? 1 : 0;
    if (result != 0 && !store32(memory, self + position_at, position + n)) return Outcome::decline;
    return Outcome::done;
}
// bool CSource::Read32<T>(T& out): four bytes through the virtual Read into a stack word, then stored little-endian.
// Only a source whose Read is the stream's is served here.
[[nodiscard]] Outcome read32(Memory& memory, std::uint32_t stream_read_address, std::uint32_t self, std::uint32_t out, std::uint32_t& result) {
    std::uint32_t table = 0, read = 0;
    if (!load32(memory, self, table) || !load32(memory, table + read_slot, read) || read != stream_read_address) return Outcome::decline;
    std::uint32_t file = 0, position = 0, size = 0, data = 0, value = 0;
    if (!load32(memory, self + file_at, file) || !load32(memory, self + position_at, position) || !load32(memory, file + size_at, size))
        return Outcome::decline;
    if (position + 4 > size) { result = 0; return Outcome::done; }
    if (!load32(memory, file + data_at, data) || !load32(memory, data + position, value) ||
        !writable(memory, self + position_at, 4) || !writable(memory, out, 4))
        return Outcome::decline;
    if (!store32(memory, self + position_at, position + 4) || !store32(memory, out, value)) return Outcome::decline;
    result = 1;
    return Outcome::done;
}

bool read_inline(CpuState& cpu, Memory& memory) noexcept {
    std::uint32_t result = 0;
    if (stream_read(memory, static_cast<std::uint32_t>(cpu.registers[0]), static_cast<std::uint32_t>(cpu.registers[1]),
                    static_cast<std::uint32_t>(cpu.registers[2]), result) != Outcome::done)
        return false;
    cpu.registers[0] = result;
    return true;
}
bool skip_inline(CpuState& cpu, Memory& memory) noexcept {
    std::uint32_t result = 0;
    if (stream_skip(memory, static_cast<std::uint32_t>(cpu.registers[0]), static_cast<std::uint32_t>(cpu.registers[1]), result) != Outcome::done)
        return false;
    cpu.registers[0] = result;
    return true;
}

// The code the members must be. 0 in `mask` words: a branch whose offset is checked separately.
constexpr std::array<std::uint32_t, 24> read_code{
    0xe92d40f8, 0xe5907008, 0xe1a04000, 0xe5900004, 0xe0827007, 0xe1a06001, 0xe1a05002, 0xeb000000, 0xe1570000, 0x8a00000b,
    0xe5940004, 0xeb000000, 0xe5941008, 0xe1a02005, 0xe0801001, 0xe1a00006, 0xeb000000, 0xe5943008, 0xe3a00001, 0xe0835005,
    0xe5845008, 0xe8bd80f8, 0xe3a00000, 0xe8bd80f8};
constexpr std::array<std::uint32_t, 13> skip_code{
    0xe92d4070, 0xe5906008, 0xe1a04000, 0xe5900004, 0xe1a05001, 0xe0816006, 0xeb000000, 0xe1560000, 0x95943008, 0x93a00001,
    0x83a00000, 0x90835005, 0x95845008};
constexpr std::array<std::uint32_t, 20> read32_code{
    0xe92d4013, 0xe5903000, 0xe1a04001, 0xe3a02004, 0xe5933008, 0xe28d1004, 0xe12fff33, 0xe3500000, 0x0a000008, 0xe5dd3006,
    0xe5dd2007, 0xe1a03803, 0xe1833c02, 0xe5dd2004, 0xe1833002, 0xe5dd2005, 0xe1833402, 0xe5843000, 0xe28dd008, 0xe8bd8010};
constexpr std::array<std::uint32_t, 11> marker_code{
    0xe92d4038, 0xe1a04000, 0xe1a05002, 0xeb000000, 0xe3500000, 0x0a000003, 0xe1a00004, 0xe1a01005, 0xe8bd4038, 0xea000000,
    0xe8bd8038};
constexpr std::array<std::uint32_t, 2> size_code{0xe5900008, 0xe12fff1e}, data_code{0xe590000c, 0xe12fff1e};

// Matches `code` at `address`; branch words (offset bits zero in `code`) are matched on their top byte and their
// targets returned in order.
template <std::size_t N>
[[nodiscard]] bool matches(Memory& memory, std::uint32_t address, const std::array<std::uint32_t, N>& code, std::vector<std::uint32_t>* targets = nullptr) {
    if ((address & 3U) != 0) return false;
    for (std::size_t i = 0; i < N; ++i) {
        std::uint32_t word = 0;
        if (!load32(memory, address + 4 * static_cast<std::uint32_t>(i), word)) return false;
        const bool branch = (code[i] & 0x0e000000U) == 0x0a000000U && (code[i] & 0x00ffffffU) == 0;
        if (branch) {
            if ((word & 0xff000000U) != code[i]) return false;
            const auto offset = static_cast<std::int32_t>(word << 8) >> 6;
            if (targets) targets->push_back(address + 4 * static_cast<std::uint32_t>(i) + 8 + static_cast<std::uint32_t>(offset));
        } else if (word != code[i]) {
            return false;
        }
    }
    return true;
}
}

NativeSet pod_source_set() {
    NativeSet set;
    set.name = "POD model source";
    auto stream_read_address = std::make_shared<std::uint32_t>(0);
    ImportService read = [](ImportCall& call) -> Error {
        std::uint32_t result = 0;
        if (pod::stream_read(call.runtime().memory, call.arg(0), call.arg(1), call.arg(2), result) != pod::Outcome::done) call.decline();
        else call.set_result(result);
        return std::nullopt;
    };
    ImportService skip = [](ImportCall& call) -> Error {
        std::uint32_t result = 0;
        if (pod::stream_skip(call.runtime().memory, call.arg(0), call.arg(1), result) != pod::Outcome::done) call.decline();
        else call.set_result(result);
        return std::nullopt;
    };
    ImportService read32 = [stream_read_address](ImportCall& call) -> Error {
        std::uint32_t result = 0;
        if (pod::read32(call.runtime().memory, *stream_read_address, call.arg(0), call.arg(1), result) != pod::Outcome::done) call.decline();
        else call.set_result(result);
        return std::nullopt;
    };
    // bool CSource::ReadMarker(unsigned& name, unsigned& length): two Read32 calls; a guest fallback for either would
    // have to resume halfway, so the whole call goes to the guest unless both are served.
    ImportService marker = [stream_read_address](ImportCall& call) -> Error {
        Memory& memory = call.runtime().memory;
        const std::uint32_t self = call.arg(0), name = call.arg(1), length = call.arg(2);
        std::uint32_t table = 0, routine = 0, file = 0, position = 0, size = 0;
        if (!load32(memory, self, table) || !load32(memory, table + pod::read_slot, routine) || routine != *stream_read_address ||
            !load32(memory, self + pod::file_at, file) || !load32(memory, self + pod::position_at, position) ||
            !load32(memory, file + pod::size_at, size) || !pod::writable(memory, name, 4) || !pod::writable(memory, length, 4)) {
            call.decline();
            return std::nullopt;
        }
        std::uint32_t first = 0, second = 0;
        if (pod::read32(memory, *stream_read_address, self, name, first) != pod::Outcome::done) return "ReadMarker: first read failed";
        if (first != 0 && pod::read32(memory, *stream_read_address, self, length, second) != pod::Outcome::done) return "ReadMarker: second read failed";
        call.set_result(first != 0 ? second : 0);
        return std::nullopt;
    };
    set.members.push_back({"pod_source_read", {"_ZN13CSourceStream4ReadEPvj"}, read, true, pod::read_inline});
    set.members.push_back({"pod_source_skip", {"_ZN13CSourceStream4SkipEj"}, skip, true, pod::skip_inline});
    // Read32 for unsigned, int and float: the same code three times.
    set.members.push_back({"pod_source_read32", {"_ZN7CSource6Read32IjEEbRT_"}, read32, true});
    set.members.push_back({"pod_source_read32_int", {"_ZN7CSource6Read32IiEEbRT_"}, read32, false});
    set.members.push_back({"pod_source_read32_float", {"_ZN7CSource6Read32IfEEbRT_"}, read32, false});
    set.members.push_back({"pod_source_marker", {"_ZN7CSource10ReadMarkerERjS0_"}, marker, false});
    set.inspect = [stream_read_address](Memory& memory, const std::map<std::string, std::uint32_t>& located) {
        const auto at = [&](const char* name) { const auto it = located.find(name); return it == located.end() ? 1U : it->second; };
        std::vector<std::uint32_t> read_calls, skip_calls, marker_calls;
        if (!pod::matches(memory, at("pod_source_read"), pod::read_code, &read_calls) || read_calls.size() != 3 ||
            !pod::matches(memory, read_calls[0], pod::size_code) || !pod::matches(memory, read_calls[1], pod::data_code) ||
            !pod::matches(memory, at("pod_source_skip"), pod::skip_code, &skip_calls) || skip_calls.size() != 1 || skip_calls[0] != read_calls[0] ||
            !pod::matches(memory, at("pod_source_read32"), pod::read32_code))
            return false;
        for (const char* other : {"pod_source_read32_int", "pod_source_read32_float"})
            if (located.count(other) != 0 && !pod::matches(memory, at(other), pod::read32_code)) return false;
        if (located.count("pod_source_marker") != 0 &&
            (!pod::matches(memory, at("pod_source_marker"), pod::marker_code, &marker_calls) || marker_calls.size() != 2 ||
             marker_calls[0] != at("pod_source_read32") || marker_calls[1] != at("pod_source_read32")))
            return false;
        *stream_read_address = at("pod_source_read");
        return true;
    };
    // Fake stream objects over a small file in scratch memory, read and skipped at positions in and past the end.
    set.verify = [stream_read_address](NativeProbe& probe, const NativeSet::Call& guest, const NativeSet::Call& native) {
        Memory& memory = probe.memory();
        const std::uint32_t base = probe.scratch(), file = base, self = base + 0x40, table = base + 0x80, out = base + 0xc0,
                            to = base + 0x100, data = base + 0x1000;
        constexpr std::uint32_t size = 37;
        std::vector<std::uint8_t> bytes(64);
        Random random;
        for (auto& byte : bytes) byte = static_cast<std::uint8_t>(random.next());
        const auto reset = [&](std::uint32_t position) {
            const std::vector<std::uint8_t> clear(0x200, 0xa5);
            return store(memory, base, clear) && store(memory, data, bytes) && store32(memory, file + pod::size_at, size) &&
                   store32(memory, file + pod::data_at, data) && store32(memory, self, table) && store32(memory, self + pod::file_at, file) &&
                   store32(memory, self + pod::position_at, position) && store32(memory, table + pod::read_slot, *stream_read_address);
        };
        const auto state = [&](std::vector<std::uint8_t>& snapshot) { snapshot.assign(0x200, 0); return load(memory, base, snapshot); };
        struct Case { const char* member; std::uint32_t position, n; };
        std::vector<Case> cases;
        for (const std::uint32_t position : {0U, 1U, 5U, 33U, 34U, 36U, 37U, 40U, 0xfffffffeU})
            for (const std::uint32_t n : {0U, 1U, 3U, 4U, 8U, 36U, 37U, 0xfffffff0U}) {
                if (n <= 64 && position <= size) cases.push_back({"pod_source_read", position, n});
                cases.push_back({"pod_source_skip", position, n});
            }
        for (const std::uint32_t position : {0U, 1U, 32U, 33U, 34U, 36U, 37U, 41U}) {
            cases.push_back({"pod_source_read32", position, 0});
            cases.push_back({"pod_source_marker", position, 0});
        }
        for (const auto& c : cases) {
            std::optional<NativeProbe::Result> returned[2];
            std::vector<std::uint8_t> after[2];
            for (int side = 0; side < 2; ++side) {
                if (!reset(c.position)) return false;
                Words arguments{self, 0, 0};
                if (std::string_view(c.member) == "pod_source_read") arguments = {self, to, c.n};
                else if (std::string_view(c.member) == "pod_source_skip") arguments = {self, c.n};
                else if (std::string_view(c.member) == "pod_source_read32") arguments = {self, out};
                else arguments = {self, out, out + 4};
                returned[side] = (side == 0 ? guest : native)(c.member, arguments);
                if (!returned[side] && std::string_view(c.member) == "pod_source_marker") break;  // marker not located
                if (!returned[side] || !state(after[side])) return false;
            }
            if (!returned[0] && !returned[1]) continue;
            if (!returned[0] || !returned[1] || (returned[0]->r0 & 0xff) != (returned[1]->r0 & 0xff) || after[0] != after[1]) return false;
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
    sets.push_back(pod_source_set());
    return sets;
}
}
