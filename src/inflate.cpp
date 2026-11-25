// SPDX-License-Identifier: GPL-2.0-or-later
// DEFLATE decoder written from RFC 1950/1951.
#include "atlan/inflate.hpp"
#include <algorithm>
#include <cstring>

namespace atlan {
std::uint32_t adler32(std::uint32_t adler, std::span<const std::uint8_t> data) noexcept {
    std::uint32_t a = adler & 0xffffU, b = adler >> 16;
    std::size_t at = 0;
    while (at < data.size()) {
        const std::size_t end = std::min(data.size(), at + 5552);  // the sums cannot overflow 32 bits before this
        for (; at < end; ++at) { a += data[at]; b += a; }
        a %= 65521U;
        b %= 65521U;
    }
    return b << 16 | a;
}
std::uint32_t crc32(std::uint32_t crc, std::span<const std::uint8_t> data) noexcept {
    static const auto table = [] {
        std::array<std::uint32_t, 256> t{};
        for (std::uint32_t i = 0; i < 256; ++i) {
            std::uint32_t c = i;
            for (int k = 0; k < 8; ++k) c = (c & 1U) ? 0xedb88320U ^ (c >> 1) : c >> 1;
            t[i] = c;
        }
        return t;
    }();
    crc = ~crc;
    for (const std::uint8_t byte : data) crc = table[(crc ^ byte) & 255U] ^ (crc >> 8);
    return ~crc;
}

namespace {
constexpr unsigned fast_bits = 10;
constexpr unsigned max_bits = 15;
constexpr std::size_t window_size = 32768;

// Canonical Huffman code. Codes of up to fast_bits bits resolve through `fast` (indexed by the next bits of
// the stream, which hold the code bit-reversed); longer ones walk the canonical counts.
struct Code {
    std::array<std::uint16_t, 1U << fast_bits> fast{};  // symbol << 4 | length; 0: long or unused code
    std::array<std::uint16_t, max_bits + 1> count{};
    std::array<std::uint16_t, 288> symbol{};
    // False if the lengths oversubscribe the code space.
    bool build(const std::uint8_t* lengths, unsigned n) noexcept {
        fast.fill(0);
        count.fill(0);
        for (unsigned i = 0; i < n; ++i) ++count[lengths[i]];
        count[0] = 0;
        int left = 1;
        for (unsigned len = 1; len <= max_bits; ++len) {
            left = left * 2 - count[len];
            if (left < 0) return false;
        }
        std::array<std::uint16_t, max_bits + 2> offset{}, next{};
        for (unsigned len = 1; len <= max_bits; ++len) offset[len + 1] = static_cast<std::uint16_t>(offset[len] + count[len]);
        unsigned code = 0;
        for (unsigned len = 1; len <= max_bits; ++len) {
            code = (code + count[len - 1]) << 1;
            next[len] = static_cast<std::uint16_t>(code);
        }
        for (unsigned i = 0; i < n; ++i) {
            const unsigned len = lengths[i];
            if (len == 0) continue;
            symbol[offset[len]++] = static_cast<std::uint16_t>(i);
            const unsigned canonical = next[len]++;
            if (len > fast_bits) continue;
            unsigned reversed = 0;
            for (unsigned b = 0; b < len; ++b) reversed |= ((canonical >> b) & 1U) << (len - 1 - b);
            for (unsigned index = reversed; index < (1U << fast_bits); index += 1U << len)
                fast[index] = static_cast<std::uint16_t>(i << 4 | len);
        }
        return true;
    }
};
constexpr std::array<std::uint16_t, 29> length_base{3, 4, 5, 6, 7, 8, 9, 10, 11, 13, 15, 17, 19, 23, 27, 31, 35, 43, 51, 59,
                                                    67, 83, 99, 115, 131, 163, 195, 227, 258};
constexpr std::array<std::uint8_t, 29> length_extra{0, 0, 0, 0, 0, 0, 0, 0, 1, 1, 1, 1, 2, 2, 2, 2, 3, 3, 3, 3, 4, 4, 4, 4, 5, 5, 5, 5, 0};
constexpr std::array<std::uint16_t, 30> distance_base{1, 2, 3, 4, 5, 7, 9, 13, 17, 25, 33, 49, 65, 97, 129, 193, 257, 385, 513, 769,
                                                      1025, 1537, 2049, 3073, 4097, 6145, 8193, 12289, 16385, 24577};
constexpr std::array<std::uint8_t, 30> distance_extra{0, 0, 0, 0, 1, 1, 2, 2, 3, 3, 4, 4, 5, 5, 6, 6, 7, 7, 8, 8, 9, 9, 10, 10, 11, 11, 12, 12, 13, 13};
constexpr std::array<std::uint8_t, 19> order{16, 17, 18, 0, 8, 7, 9, 6, 10, 5, 11, 4, 12, 3, 13, 2, 14, 1, 15};

enum class Phase : std::uint8_t {
    zlib_header, block_header, stored_length, stored, dynamic_counts, dynamic_order, dynamic_lengths,
    codes, match, trailer, done, bad, dictionary
};
// Bits not yet used, oldest in the low end. A reader takes from a copy and the caller commits the copy only
// when a whole unit could be read, so a unit cut short by the end of the input leaves no trace.
struct Bits {
    std::uint64_t value{};
    unsigned count{};
    [[nodiscard]] bool take(unsigned n, std::uint32_t& out) noexcept {
        if (count < n) return false;
        out = static_cast<std::uint32_t>(value & ((std::uint64_t{1} << n) - 1));
        value >>= n;
        count -= n;
        return true;
    }
    // 1: decoded, 0: more bits needed, -1: no such code.
    [[nodiscard]] int decode(const Code& code, std::uint32_t& out) noexcept {
        const std::uint16_t entry = code.fast[value & ((1U << fast_bits) - 1)];
        if (entry != 0) {
            const unsigned len = entry & 15U;
            if (len > count) return 0;
            out = entry >> 4;
            value >>= len;
            count -= len;
            return 1;
        }
        unsigned walked = 0;
        int first = 0, index = 0, bits = 0;
        for (unsigned len = 1; len <= max_bits; ++len) {
            if (len > count) return 0;
            bits |= static_cast<int>((value >> walked) & 1U);
            ++walked;
            const int n = code.count[len];
            if (bits - n < first) {
                out = code.symbol[static_cast<std::size_t>(index + (bits - first))];
                value >>= len;
                count -= len;
                return 1;
            }
            index += n;
            first = (first + n) << 1;
            bits <<= 1;
        }
        return -1;
    }
};
}

struct Inflater::State {
    bool framed{};
    Phase phase{};
    Bits bits;
    bool last{};
    std::uint32_t stored_left{};
    std::uint32_t literals{}, distances{}, order_count{}, index{};  // dynamic block header progress
    std::array<std::uint8_t, 320> lengths{};
    Code order_code, literal_code, distance_code;
    std::uint32_t match_length{}, match_distance{};
    std::uint64_t total{};
    std::uint32_t adler{1};
    std::size_t window_at{};
    std::array<std::uint8_t, window_size> window{};

    void clear(bool zlib_framing) noexcept {
        framed = zlib_framing;
        phase = framed ? Phase::zlib_header : Phase::block_header;
        bits = {};
        last = false;
        total = 0;
        adler = 1;
        window_at = 0;
        match_length = 0;
    }
};

Inflater::Inflater(bool zlib_framing) : state_(std::make_unique<State>()) { state_->clear(zlib_framing); }
Inflater::~Inflater() = default;
Inflater::Inflater(Inflater&&) noexcept = default;
Inflater& Inflater::operator=(Inflater&&) noexcept = default;
void Inflater::reset() noexcept { state_->clear(state_->framed); }
std::uint32_t Inflater::checksum() const noexcept { return state_->adler; }

Inflater::Progress Inflater::run(std::span<const std::uint8_t> input, std::span<std::uint8_t> output) noexcept {
    State& s = *state_;
    static const struct Fixed {
        Code literal, distance;
        Fixed() {
            std::array<std::uint8_t, 288> l{};
            for (unsigned i = 0; i < 288; ++i) l[i] = i < 144 ? 8 : i < 256 ? 9 : i < 280 ? 7 : 8;
            literal.build(l.data(), 288);
            std::array<std::uint8_t, 30> d{};
            d.fill(5);
            distance.build(d.data(), 30);
        }
    } fixed;
    std::size_t in = 0, out = 0;
    const auto refill = [&] {
        while (s.bits.count <= 56 && in < input.size()) {
            s.bits.value |= std::uint64_t{input[in++]} << s.bits.count;
            s.bits.count += 8;
        }
    };
    const auto emit = [&](std::uint8_t byte) {
        output[out++] = byte;
        s.window[s.window_at] = byte;
        s.window_at = (s.window_at + 1) & (window_size - 1);
    };
    Stop stop = Stop::input;
    bool running = true;
    while (running) {
        refill();
        Bits b = s.bits;  // committed with `s.bits = b` once a whole unit was read
        std::uint32_t v = 0;
        switch (s.phase) {
        case Phase::zlib_header: {
            if (!b.take(16, v)) { running = false; break; }
            const std::uint32_t cmf = v & 255U, flags = v >> 8;
            if ((cmf & 15U) != 8 || (cmf >> 4) > 7 || (cmf * 256 + flags) % 31 != 0) { s.phase = Phase::bad; break; }
            s.bits = b;
            s.phase = (flags & 0x20U) ? Phase::dictionary : Phase::block_header;
            break;
        }
        case Phase::block_header: {
            if (s.last) {
                // Whole bytes follow the last block.
                s.bits.value >>= s.bits.count & 7U;
                s.bits.count &= ~7U;
                s.phase = s.framed ? Phase::trailer : Phase::done;
                break;
            }
            if (!b.take(3, v)) { running = false; break; }
            s.last = (v & 1U) != 0;
            const std::uint32_t type = v >> 1;
            if (type == 0) {
                b.value >>= b.count & 7U;
                b.count &= ~7U;
                s.phase = Phase::stored_length;
            } else if (type == 1) {
                s.phase = Phase::codes;
                s.literal_code = fixed.literal;
                s.distance_code = fixed.distance;
            } else if (type == 2) {
                s.phase = Phase::dynamic_counts;
            } else {
                s.phase = Phase::bad;
                break;
            }
            s.bits = b;
            break;
        }
        case Phase::stored_length: {
            if (!b.take(32, v)) { running = false; break; }
            if ((v & 0xffffU) != (~v >> 16)) { s.phase = Phase::bad; break; }
            s.bits = b;
            s.stored_left = v & 0xffffU;
            s.phase = Phase::stored;
            break;
        }
        case Phase::stored: {
            if (s.stored_left == 0) { s.phase = Phase::block_header; break; }
            if (out == output.size()) { stop = Stop::output; running = false; break; }
            if (!b.take(8, v)) { running = false; break; }
            s.bits = b;
            emit(static_cast<std::uint8_t>(v));
            --s.stored_left;
            break;
        }
        case Phase::dynamic_counts: {
            if (!b.take(14, v)) { running = false; break; }
            s.literals = (v & 31U) + 257;
            s.distances = ((v >> 5) & 31U) + 1;
            s.order_count = (v >> 10) + 4;
            if (s.literals > 286 || s.distances > 30) { s.phase = Phase::bad; break; }
            s.bits = b;
            s.lengths.fill(0);
            s.index = 0;
            s.phase = Phase::dynamic_order;
            break;
        }
        case Phase::dynamic_order: {
            if (s.index == s.order_count) {
                std::array<std::uint8_t, 19> l{};
                std::copy_n(s.lengths.begin(), 19, l.begin());
                if (!s.order_code.build(l.data(), 19)) { s.phase = Phase::bad; break; }
                s.lengths.fill(0);
                s.index = 0;
                s.phase = Phase::dynamic_lengths;
                break;
            }
            if (!b.take(3, v)) { running = false; break; }
            s.bits = b;
            s.lengths[order[s.index++]] = static_cast<std::uint8_t>(v);
            break;
        }
        case Phase::dynamic_lengths: {
            const std::uint32_t wanted = s.literals + s.distances;
            if (s.index == wanted) {
                if (s.lengths[256] == 0 || !s.literal_code.build(s.lengths.data(), s.literals) ||
                    !s.distance_code.build(s.lengths.data() + s.literals, s.distances)) { s.phase = Phase::bad; break; }
                s.phase = Phase::codes;
                break;
            }
            std::uint32_t symbol = 0;
            const int got = b.decode(s.order_code, symbol);
            if (got == 0) { running = false; break; }
            if (got < 0) { s.phase = Phase::bad; break; }
            if (symbol < 16) {
                s.bits = b;
                s.lengths[s.index++] = static_cast<std::uint8_t>(symbol);
                break;
            }
            std::uint32_t repeat = 0;
            std::uint8_t value = 0;
            if (symbol == 16) {
                if (s.index == 0) { s.phase = Phase::bad; break; }
                if (!b.take(2, repeat)) { running = false; break; }
                repeat += 3;
                value = s.lengths[s.index - 1];
            } else if (symbol == 17) {
                if (!b.take(3, repeat)) { running = false; break; }
                repeat += 3;
            } else {
                if (!b.take(7, repeat)) { running = false; break; }
                repeat += 11;
            }
            if (s.index + repeat > wanted) { s.phase = Phase::bad; break; }
            s.bits = b;
            while (repeat-- != 0) s.lengths[s.index++] = value;
            break;
        }
        case Phase::codes: {
            std::uint32_t symbol = 0;
            int got = b.decode(s.literal_code, symbol);
            if (got == 0) { running = false; break; }
            if (got < 0) { s.phase = Phase::bad; break; }
            if (symbol < 256) {
                // Only a unit that produces output stops for room, so the end of a block or of the stream is
                // still reached when the output is exactly full.
                if (out == output.size()) { stop = Stop::output; running = false; break; }
                s.bits = b;
                emit(static_cast<std::uint8_t>(symbol));
                break;
            }
            if (symbol == 256) {
                s.bits = b;
                s.phase = Phase::block_header;
                break;
            }
            symbol -= 257;
            if (symbol >= 29) { s.phase = Phase::bad; break; }
            std::uint32_t extra = 0;
            if (!b.take(length_extra[symbol], extra)) { running = false; break; }
            const std::uint32_t length = length_base[symbol] + extra;
            got = b.decode(s.distance_code, symbol);
            if (got == 0) { running = false; break; }
            if (got < 0 || symbol >= 30) { s.phase = Phase::bad; break; }
            if (!b.take(distance_extra[symbol], extra)) { running = false; break; }
            const std::uint32_t distance = distance_base[symbol] + extra;
            if (distance > s.total + out) { s.phase = Phase::bad; break; }
            s.bits = b;
            s.match_length = length;
            s.match_distance = distance;
            s.phase = Phase::match;
            [[fallthrough]];
        }
        case Phase::match: {
            while (s.match_length != 0 && out < output.size()) {
                emit(s.window[(s.window_at - s.match_distance) & (window_size - 1)]);
                --s.match_length;
            }
            if (s.match_length != 0) { stop = Stop::output; running = false; break; }
            s.phase = Phase::codes;
            break;
        }
        case Phase::trailer: {
            if (!b.take(32, v)) { running = false; break; }
            s.bits = b;
            const std::uint32_t stored = (v & 255U) << 24 | (v & 0xff00U) << 8 | ((v >> 8) & 0xff00U) | v >> 24;
            const std::uint32_t computed = atlan::adler32(s.adler, output.first(out));
            s.phase = stored == computed ? Phase::done : Phase::bad;
            break;
        }
        case Phase::done: stop = Stop::end; running = false; break;
        case Phase::bad: stop = Stop::error; running = false; break;
        case Phase::dictionary: stop = Stop::dictionary; running = false; break;
        }
    }
    if (stop != Stop::input) {
        // Hand back the whole bytes that were fetched ahead in this call and not used.
        const std::size_t back = std::min<std::size_t>(s.bits.count / 8, in);
        in -= back;
        s.bits.count -= static_cast<unsigned>(back * 8);
        s.bits.value &= s.bits.count == 0 ? 0 : (~std::uint64_t{0} >> (64 - s.bits.count));
    }
    s.adler = atlan::adler32(s.adler, output.first(out));
    s.total += out;
    return {in, out, stop};
}
}
