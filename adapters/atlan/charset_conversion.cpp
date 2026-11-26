// SPDX-License-Identifier: GPL-2.0-or-later
#include "adapters/atlan/charset_conversion.hpp"
#include "atlan/text_codec.hpp"
#include <algorithm>
#include <array>
#include <random>
#include <string>
#include <vector>

namespace atlan::shell::charset {
namespace {
using Words = std::vector<std::uint32_t>;

// User-defined area: two rows of 94 codes that map in order onto the start of the Private Use Area.
constexpr std::uint8_t user_rows[2] = {0xC9, 0xFE};
constexpr std::uint8_t row_first = 0xA1, row_last = 0xFE;
constexpr unsigned row_size = row_last - row_first + 1;
constexpr char16_t user_first = 0xE000, user_end = user_first + 2 * row_size;
// The last code the table-driven variant's table covers.
constexpr unsigned table_last_lead = 0xFD, table_last_trail = 0xFE;

// Reads guest memory a page at a time for the converters' byte-by-byte access.
class GuestBytes {
public:
    GuestBytes(Memory& memory, std::uint32_t base) : memory_(memory), base_(base) {}
    std::optional<std::uint8_t> operator()(std::uint32_t offset) {
        const std::uint64_t at = std::uint64_t{base_} + offset;
        if (at >= 0x100000000ULL) return std::nullopt;
        const auto address = static_cast<std::uint32_t>(at);
        if (page_.empty() || address < page_base_ || address - page_base_ >= page_.size()) {
            page_ = memory_.page_span(address, MemoryAccess::read);
            if (page_.empty()) return std::nullopt;
            page_base_ = address;
        }
        return std::to_integer<std::uint8_t>(page_[address - page_base_]);
    }

private:
    Memory& memory_;
    std::uint32_t base_;
    std::span<std::byte> page_;
    std::uint32_t page_base_{};
};
class GuestOutput {
public:
    GuestOutput(Memory& memory, std::uint32_t base) : memory_(memory), at_(base) {}
    bool put(std::uint8_t byte) {
        const std::array<std::byte, 1> one{std::byte{byte}};
        if (!memory_.write(at_, one)) return false;
        ++at_;
        return true;
    }

private:
    Memory& memory_;
    std::uint32_t at_;
};

// The common head of both entry points: (code page, flags, input, input length, output, output length).
struct Arguments {
    std::uint32_t input{}, output{}, output_length{};
    std::int32_t input_length{};
};
std::optional<Arguments> read_arguments(ImportCall& call) {
    Arguments a;
    if (!call.read_arg(4, a.output) || !call.read_arg(5, a.output_length)) return std::nullopt;
    a.input = call.arg(2);
    a.input_length = static_cast<std::int32_t>(call.arg(3));
    return a;
}

ImportService to_wide_service() {
    return [](ImportCall& call) -> std::optional<std::string> {
        const auto a = read_arguments(call);
        if (!a) return "multibyte_to_wide: unreadable arguments";
        Memory& memory = call.runtime().memory;
        GuestBytes in(memory, a->input);
        std::uint32_t in_bytes = 0;
        if (a->input_length == -1) {  // NUL-terminated: the terminator is converted too
            while (true) {
                const auto byte = in(in_bytes);
                if (!byte) return "multibyte_to_wide: unreadable input";
                ++in_bytes;
                if (*byte == 0) break;
            }
        } else if (a->input_length > 0) {
            in_bytes = static_cast<std::uint32_t>(a->input_length);
        } else {
            call.set_result(0);
            return std::nullopt;
        }
        // Length 0 asks how long the result would be; the converter then writes into its own buffer.
        const bool measure = a->output_length == 0;
        const std::uint32_t out_bytes = measure ? in_bytes * 2 : a->output_length * 2;
        if (a->input == 0) {  // a null input only resets the converter
            call.set_result(0);
            return std::nullopt;
        }
        GuestOutput out(memory, a->output);
        const auto left = to_wide(std::ref(in), in_bytes, [&](std::uint16_t unit) {
            return measure || (out.put(static_cast<std::uint8_t>(unit & 0xFF)) && out.put(static_cast<std::uint8_t>(unit >> 8)));
        }, out_bytes);
        if (!left) return "multibyte_to_wide: unreadable input or unwritable output";
        call.set_result((out_bytes - *left) >> 1);
        return std::nullopt;
    };
}

ImportService to_multibyte_service() {
    return [](ImportCall& call) -> std::optional<std::string> {
        const auto a = read_arguments(call);
        if (!a) return "wide_to_multibyte: unreadable arguments";
        Memory& memory = call.runtime().memory;
        GuestBytes bytes(memory, a->input);
        const auto unit_at = [&](std::uint32_t offset) -> std::optional<std::uint16_t> {
            const auto low = bytes(offset), high = low ? bytes(offset + 1) : std::nullopt;
            if (!high) return std::nullopt;
            return static_cast<std::uint16_t>(*low | (*high << 8));
        };
        std::uint32_t in_bytes = 0;
        if (a->input_length == -1) {  // NUL-terminated: the terminator is converted too
            while (true) {
                const auto unit = unit_at(in_bytes);
                if (!unit) return "wide_to_multibyte: unreadable input";
                in_bytes += 2;
                if (*unit == 0) break;
            }
        } else if (a->input_length > 0) {
            in_bytes = static_cast<std::uint32_t>(a->input_length) * 2;
        } else {
            call.set_result(0);
            return std::nullopt;
        }
        const bool measure = a->output_length == 0;
        const std::uint32_t out_bytes = measure ? in_bytes : a->output_length;
        if (a->input == 0) {
            call.set_result(0);
            return std::nullopt;
        }
        GuestOutput out(memory, a->output);
        const auto left = to_multibyte(unit_at, in_bytes, [&](std::uint8_t byte) { return measure || out.put(byte); }, out_bytes);
        if (!left) return "wide_to_multibyte: unreadable input or unwritable output";
        call.set_result(out_bytes - *left);
        return std::nullopt;
    };
}

// --- verification ---

void put_word(std::vector<std::byte>& out, std::uint16_t unit) {
    out.push_back(std::byte(unit & 0xFF));
    out.push_back(std::byte(unit >> 8));
}
std::uint32_t u(std::int32_t v) { return static_cast<std::uint32_t>(v); }

// Runs one call as guest code and as the host service on the same input and compares the result and the output.
class Comparison {
public:
    Comparison(NativeProbe& probe, const NativeSet::Call& guest, const NativeSet::Call& native)
        : probe_(probe), guest_(guest), native_(native) {}
    static constexpr std::uint32_t input_offset = 0, output_offset = 0x20000, area = 0x20000;

    // `output_length` in the entry point's units; `output_bytes` how much of the output area to compare.
    bool same(const char* name, std::span<const std::byte> input, std::int32_t input_length, std::uint32_t output_length,
              std::uint32_t output_bytes, bool null_input = false) {
        if (input.size() > area || output_bytes > area) return false;
        Memory& memory = probe_.memory();
        const std::uint32_t in_at = probe_.scratch() + input_offset, out_at = probe_.scratch() + output_offset;
        if (!memory.write(in_at, input)) return false;
        const Words arguments{0, 0, null_input ? 0U : in_at, u(input_length), out_at, output_length};
        std::uint32_t result[2] = {};
        std::vector<std::byte> written[2];
        for (unsigned run = 0; run < 2; ++run) {
            fill_.assign(output_bytes + 8, std::byte{0xA5});
            if (!memory.write(out_at, fill_)) return false;
            const auto r = run == 0 ? guest_(name, arguments) : native_(name, arguments);
            if (!r) return false;
            result[run] = r->r0;
            written[run].resize(fill_.size());
            if (!memory.read(out_at, written[run])) return false;
        }
        return result[0] == result[1] && written[0] == written[1];
    }

private:
    NativeProbe& probe_;
    const NativeSet::Call& guest_;
    const NativeSet::Call& native_;
    std::vector<std::byte> fill_;
};

std::vector<std::byte> bytes_of(std::initializer_list<unsigned> values) {
    std::vector<std::byte> out;
    for (const unsigned v : values) out.push_back(std::byte(v));
    return out;
}

bool verify(NativeProbe& probe, const NativeSet::Call& guest, const NativeSet::Call& native, bool exhaustive) {
    Comparison c(probe, guest, native);
    const char* wide = "multibyte_to_wide";
    const char* narrow = "wide_to_multibyte";
    // Whole input; output room in the entry point's units (default: enough for all of it).
    const auto to_wide_bytes = [&](const std::vector<std::byte>& in, std::optional<std::uint32_t> out_units = std::nullopt) {
        const auto room = out_units.value_or(static_cast<std::uint32_t>(in.size()) + 2);
        return c.same(wide, in, static_cast<std::int32_t>(in.size()), room, 2 * std::max<std::uint32_t>(room, static_cast<std::uint32_t>(in.size())));
    };
    const auto to_narrow_units = [&](const std::vector<std::byte>& in, std::optional<std::uint32_t> out_bytes = std::nullopt) {
        const auto room = out_bytes.value_or(static_cast<std::uint32_t>(in.size()) + 2);
        return c.same(narrow, in, static_cast<std::int32_t>(in.size() / 2), room, std::max<std::uint32_t>(room, static_cast<std::uint32_t>(in.size())));
    };

    // Cheap distinguishing cases first, so that a different converter is rejected quickly.
    if (!to_wide_bytes(bytes_of({'A', 0x80, 'B'})) || !to_wide_bytes(bytes_of({'A', 0xB0, 0xA1, 0xFF, 'B'}))) return false;
    if (!to_narrow_units(bytes_of({'A', 0, 0x80, 0, 'B', 0})) || !to_narrow_units(bytes_of({0x00, 0xAC, 0x00, 0xD8, 'B', 0})))
        return false;

    // Every character the host converts, in one run each way.
    std::vector<std::byte> all_codes, all_units;
    for (unsigned b = 1; b < 0x80; ++b) {
        all_codes.push_back(std::byte(b));
        put_word(all_units, static_cast<std::uint16_t>(b));
    }
    for (unsigned lead = 0x81; lead <= 0xFE; ++lead)
        for (unsigned trail = 0x41; trail <= 0xFE; ++trail)
            if (const auto cp = decode(static_cast<std::uint8_t>(lead), static_cast<std::uint8_t>(trail))) {
                all_codes.push_back(std::byte(lead));
                all_codes.push_back(std::byte(trail));
                put_word(all_units, *cp);
            }
    // Running out of room and measuring are checked on short text below.
    if (!to_wide_bytes(all_codes) || !to_narrow_units(all_units)) return false;

    // Lengths: NUL-terminated input, empty and negative lengths, output that ends inside a character.
    const auto korean = bytes_of({'a', 0xC7, 0xD1, 0xB1, 0xDB, 'z', 0});
    const auto korean_units = bytes_of({'a', 0, 0x5C, 0xD5, 0x00, 0xAE, 'z', 0, 0, 0});
    for (const std::int32_t length : {-1, 0, -2, 1, 2, 3, 6, 7})
        for (const std::uint32_t room : {0U, 1U, 2U, 3U, 4U, 5U, 9U}) {
            if (!c.same(wide, korean, length, room, 16) || !c.same(narrow, korean_units, length, room, 16)) return false;
        }
    // An odd trailing byte, a lead byte at the very end.
    if (!c.same(narrow, korean_units, 3, 9, 16) || !to_wide_bytes(bytes_of({'x', 0xB0}))) return false;
    // A null input with a length: the converter is only reset.
    if (!c.same(wide, korean, 4, 8, 16, true) || !c.same(narrow, korean_units, 4, 8, 16, true) || !c.same(wide, korean, 4, 0, 16, true))
        return false;

    // Codes the host rejects, each in its own run since conversion stops there.
    const auto rejected_code = [&](unsigned lead, unsigned trail) {
        return decode(static_cast<std::uint8_t>(lead), static_cast<std::uint8_t>(trail)) || to_wide_bytes(bytes_of({'A', lead, trail, 'B'}));
    };
    const auto rejected_unit = [&](unsigned unit) {
        return encode(static_cast<char16_t>(unit)) || to_narrow_units(bytes_of({'A', 0, unit & 0xFF, unit >> 8, 'B', 0}));
    };
    if (exhaustive) {
        for (unsigned lead = 0x80; lead <= 0xFF; ++lead) {
            if (!to_wide_bytes(bytes_of({'A', lead}))) return false;
            for (unsigned trail = 0; trail <= 0xFF; ++trail)
                if (!rejected_code(lead, trail)) return false;
        }
        for (unsigned unit = 0x80; unit <= 0xFFFF; ++unit)
            if (!rejected_unit(unit)) return false;
        return true;
    }
    // A sample: lead bytes alone, trail bytes around each range edge under the leads of each region, random codes
    // and code units, and the ends of the ranges.
    for (const unsigned lead : {0x80U, 0x81U, 0xA0U, 0xA1U, 0xC8U, 0xC9U, 0xFEU, 0xFFU}) {
        if (!to_wide_bytes(bytes_of({'A', lead}))) return false;
        for (const unsigned trail : {0x00U, 0x40U, 0x41U, 0x5AU, 0x5BU, 0x60U, 0x61U, 0x7AU, 0x7BU, 0x80U, 0x81U, 0xA0U, 0xA1U, 0xFEU, 0xFFU})
            if (!rejected_code(lead, trail)) return false;
    }
    for (const unsigned unit : {0x80U, 0x9FU, 0xA0U, 0xFFU, 0x327EU, 0xD7FFU, 0xD800U, 0xDBFFU, 0xDC00U, 0xDFFFU, 0xE0BBU, 0xE0BCU,
                                0xF8FFU, 0xFEFFU, 0xFFFDU, 0xFFFEU, 0xFFFFU})
        if (!rejected_unit(unit)) return false;
    std::mt19937 random(20261126);
    for (unsigned i = 0; i < 100; ++i) {
        const unsigned lead = 0x80 + random() % 0x80, trail = random() % 0x100, unit = 0x80 + random() % 0xFF80;
        if (!rejected_code(lead, trail) || !rejected_unit(unit)) return false;
    }
    return true;
}

// --- the table-driven variant ---

ImportService table_to_wide_service() {
    return [](ImportCall& call) -> std::optional<std::string> {
        const auto a = read_arguments(call);
        if (!a) return "multibyte_to_wide: unreadable arguments";
        Memory& memory = call.runtime().memory;
        GuestBytes in(memory, a->input);
        std::uint32_t in_bytes = 0;
        if (a->input_length == -1) {  // up to the NUL, which is not counted
            while (true) {
                const auto byte = in(in_bytes);
                if (!byte) return "multibyte_to_wide: unreadable input";
                if (*byte == 0) break;
                ++in_bytes;
            }
        } else if (a->input_length > 0) {
            in_bytes = static_cast<std::uint32_t>(a->input_length);
        } else {
            call.set_result(0);
            return std::nullopt;
        }
        // Length 0 asks how long the result would be; the converter then writes into its own buffer. Otherwise the
        // room it is given is twice the length.
        const bool measure = a->output_length == 0;
        const std::uint32_t room = measure ? (in_bytes + 1) * 2 : a->output_length * 2;
        const auto converted = table_to_wide(std::ref(in), in_bytes, room);
        if (!converted) return "multibyte_to_wide: unreadable input";
        const auto count = static_cast<std::uint32_t>(converted->units.size());
        if (converted->outside_table) {  // nothing changed yet: the guest's own code handles it
            call.decline();
            return std::nullopt;
        }
        if (!measure) {
            const std::uint64_t written = 2ULL * (count + (converted->terminator ? 1 : 0));
            const std::uint64_t in_end = std::uint64_t{a->input} + converted->bytes_read, out_end = std::uint64_t{a->output} + written;
            if (written != 0 && a->output < in_end && a->input < out_end) {  // the output would change input still to be read
                call.decline();
                return std::nullopt;
            }
            std::vector<std::byte> bytes;
            bytes.reserve(written);
            for (const char16_t unit : converted->units) put_word(bytes, unit);
            if (converted->terminator) put_word(bytes, 0);
            if (!bytes.empty() && !memory.write(a->output, bytes)) return "multibyte_to_wide: unwritable output";
        }
        call.set_result(static_cast<std::int32_t>(count) < 0 ? 0 : count);
        return std::nullopt;
    };
}

bool verify_table(NativeProbe& probe, const NativeSet::Call& guest, const NativeSet::Call& native) {
    Comparison c(probe, guest, native);
    const char* wide = "multibyte_to_wide";
    // Distinguishes it from the converter that stops: 0x80 stands for itself here.
    if (!c.same(wide, bytes_of({'A', 0x80, 'B', 0}), 3, 8, 24)) return false;
    // Every code the table covers, in one run (a code with a trail byte of 0 too: the trail is not checked).
    std::vector<std::byte> all;
    for (unsigned lead = 1; lead <= 0x80; ++lead) all.push_back(std::byte(lead));
    for (unsigned lead = 0x81; lead <= table_last_lead; ++lead)
        for (unsigned trail = 0; trail <= 0xFF; ++trail)
            if (lead != table_last_lead || trail <= table_last_trail) {
                all.push_back(std::byte(lead));
                all.push_back(std::byte(trail));
            }
    all.push_back(std::byte{0});
    const auto size = static_cast<std::uint32_t>(all.size());
    if (!c.same(wide, all, static_cast<std::int32_t>(size - 1), size, 2 * size + 8)) return false;
    // Lengths: NUL-terminated, empty and negative, odd counts that run on to the NUL, little room, measuring.
    const auto korean = bytes_of({'a', 0xC7, 0xD1, 0xB1, 0xDB, 'z', 0, 'q', 0});
    for (const std::int32_t length : {-1, 0, -2, 1, 2, 3, 4, 6, 7})
        for (const std::uint32_t room : {0U, 1U, 2U, 3U, 9U})
            if (!c.same(wide, korean, length, room, 32)) return false;
    // A lead byte whose trail is the NUL: the NUL is taken as the trail and conversion runs on.
    if (!c.same(wide, bytes_of({'x', 0xB0, 0, 'y', 0}), -1, 8, 32) || !c.same(wide, bytes_of({'x', 0xB0, 0, 'y', 0}), 2, 8, 32)) return false;
    return true;
}
}

std::optional<char16_t> decode(std::uint8_t lead, std::uint8_t trail) noexcept {
    if (lead < 0x80) return lead;
    if (const char16_t cp = text::cp949_decode(lead, trail)) return cp;
    for (unsigned row = 0; row < 2; ++row)
        if (lead == user_rows[row] && trail >= row_first && trail <= row_last)
            return static_cast<char16_t>(user_first + row * row_size + (trail - row_first));
    return std::nullopt;
}

std::optional<std::uint16_t> encode(char16_t code_point) {
    if (code_point < 0x80) return code_point;
    if (code_point >= 0xD800 && code_point <= 0xDFFF) return std::nullopt;
    if (code_point >= user_first && code_point < user_end) {
        const unsigned index = code_point - user_first;
        return static_cast<std::uint16_t>((user_rows[index / row_size] << 8) | (row_first + index % row_size));
    }
    if (const std::uint16_t code = text::cp949_encode(code_point)) return code;
    return std::nullopt;
}

std::optional<std::uint32_t> to_wide(const ByteAt& in, std::uint32_t in_bytes, const PutUnit& put, std::uint32_t out_bytes) {
    std::uint32_t at = 0;
    while (at < in_bytes) {
        const auto lead = in(at);
        if (!lead) return std::nullopt;
        std::optional<char16_t> cp;
        std::uint32_t length = 1;
        if (*lead < 0x80) {
            cp = *lead;
        } else if (*lead >= 0x81 && *lead <= 0xFE) {
            if (in_bytes - at < 2) break;  // incomplete character at the end
            const auto trail = in(at + 1);
            if (!trail) return std::nullopt;
            cp = decode(*lead, *trail);
            length = 2;
        }
        if (!cp || out_bytes < 2) break;
        if (!put(*cp)) return std::nullopt;
        out_bytes -= 2;
        at += length;
    }
    return out_bytes;
}

std::optional<std::uint32_t> to_multibyte(const UnitAt& in, std::uint32_t in_bytes, const PutByte& put, std::uint32_t out_bytes) {
    for (std::uint32_t at = 0; in_bytes - at >= 2; at += 2) {
        const auto unit = in(at);
        if (!unit) return std::nullopt;
        const auto code = encode(static_cast<char16_t>(*unit));
        if (!code) break;
        const std::uint32_t length = *code < 0x80 ? 1 : 2;
        if (out_bytes < length) break;
        if (length == 2 && !put(static_cast<std::uint8_t>(*code >> 8))) return std::nullopt;
        if (!put(static_cast<std::uint8_t>(*code & 0xFF))) return std::nullopt;
        out_bytes -= length;
    }
    return out_bytes;
}

std::optional<TableConversion> table_to_wide(const ByteAt& in, std::uint32_t in_bytes, std::uint32_t room) {
    TableConversion out;
    std::uint32_t at = 0;
    while (true) {
        const auto lead = in(at);
        if (!lead) return std::nullopt;
        out.bytes_read = std::max(out.bytes_read, at + 1);
        if (*lead == 0 || room == 0 || in_bytes == 0) break;
        char16_t unit = *lead;
        if (*lead <= 0x80) {
            ++at;
            in_bytes -= 1;
        } else {
            const auto trail = in(at + 1);
            if (!trail) return std::nullopt;
            out.bytes_read = std::max(out.bytes_read, at + 2);
            if (*lead > table_last_lead || (*lead == table_last_lead && *trail > table_last_trail)) {
                out.outside_table = true;
                return out;
            }
            at += 2;
            in_bytes -= 2;  // an odd count passes 0 and runs on to a NUL
            unit = text::cp949_decode(*lead, *trail);
            if (unit == 0) unit = u'?';
        }
        out.units.push_back(unit);
        --room;
    }
    out.terminator = room != 0;
    return out;
}

std::vector<NativeSet> conversion_sets(bool exhaustive) {
    std::vector<NativeSet> sets(2);
    NativeSet& set = sets[0];
    set.name = "text conversion (iconv)";
    set.members.push_back({"multibyte_to_wide", {}, to_wide_service(), true});
    set.members.push_back({"wide_to_multibyte", {}, to_multibyte_service(), true});
    set.verify = [exhaustive](NativeProbe& probe, const NativeSet::Call& guest, const NativeSet::Call& native) {
        return verify(probe, guest, native, exhaustive);
    };
    // Builds whose converter looks codes up in a table (only the direction that matters for start-up time).
    NativeSet& table = sets[1];
    table.name = "text conversion (table)";
    table.members.push_back({"multibyte_to_wide", {}, table_to_wide_service(), true});
    table.verify = verify_table;
    return sets;
}
}
