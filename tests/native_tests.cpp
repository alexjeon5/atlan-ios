// SPDX-License-Identifier: GPL-2.0-or-later
// Host replacements of guest functions: the decoder, the services on guest memory, and locating, verifying,
// installing and declining on a synthetic guest.
#include "atlan/inflate.hpp"
#include "atlan/inflate_vectors.hpp"
#include "atlan/native_functions.hpp"
#include <array>
#include <cstdlib>
#include <iostream>
#include <random>
#include <stdexcept>

namespace {
using namespace atlan;
unsigned checks{};
void check(bool result, const char* message) {
    ++checks;
    if (!result) throw std::runtime_error(message);
}
constexpr std::uint32_t code = 0x1000, stack = 0x80000, sentinel = 0xffff0000;

std::span<const std::uint8_t> bytes_of(const std::string& text) { return {reinterpret_cast<const std::uint8_t*>(text.data()), text.size()}; }

void decoder() {
    check(adler32(1, bytes_of("Wikipedia")) == 0x11e60398, "adler32 reference value");
    check(crc32(0, bytes_of("123456789")) == 0xcbf43926, "crc32 reference value");
    check(crc32(crc32(0, bytes_of("1234")), bytes_of("56789")) == 0xcbf43926, "crc32 continues");
    struct Vector { std::span<const std::uint8_t> stream; std::string plain; };
    const Vector vectors[] = {{inflate_vectors::text_stream, inflate_vectors::text_plain()},
                              {inflate_vectors::hello_stream, inflate_vectors::hello_plain()},
                              {inflate_vectors::stored_stream, inflate_vectors::stored_plain()}};
    std::mt19937 random(7);
    for (const auto& vector : vectors) {
        // Trailing bytes after the stream must be left unconsumed.
        std::vector<std::uint8_t> input(vector.stream.begin(), vector.stream.end());
        input.insert(input.end(), {0xaa, 0xbb, 0xcc});
        for (int mode = 0; mode < 40; ++mode) {
            Inflater inflater;
            std::vector<std::uint8_t> output(vector.plain.size() + (mode == 1 ? 0 : 64));
            std::size_t in = 0, out = 0;
            Inflater::Stop stop = Inflater::Stop::input;
            for (unsigned round = 0; round < 100000 && stop != Inflater::Stop::end && stop != Inflater::Stop::error; ++round) {
                const std::size_t in_step = mode < 2 ? input.size() : mode == 2 ? 1 : random() % 40;
                const std::size_t out_step = mode < 2 ? output.size() : mode == 2 ? 1 : random() % 60;
                const auto progress = inflater.run(std::span(input).subspan(in, std::min(in_step, input.size() - in)),
                                                   std::span(output).subspan(out, std::min(out_step, output.size() - out)));
                in += progress.consumed;
                out += progress.produced;
                stop = progress.stop;
            }
            check(stop == Inflater::Stop::end, "stream ends");
            check(in == vector.stream.size(), "only the stream's own bytes are consumed");
            check(out == vector.plain.size() && std::equal(vector.plain.begin(), vector.plain.end(), output.begin(), [](char a, std::uint8_t b) {
                return static_cast<std::uint8_t>(a) == b;
            }), "decoded text");
            check(inflater.checksum() == adler32(1, bytes_of(vector.plain)), "running checksum");
        }
    }
    {
        std::vector<std::uint8_t> damaged(inflate_vectors::text_stream.begin(), inflate_vectors::text_stream.end());
        damaged[damaged.size() - 2] ^= 0x40;  // the stored Adler-32
        std::vector<std::uint8_t> output(8192);
        Inflater inflater;
        check(inflater.run(damaged, output).stop == Inflater::Stop::error, "checksum mismatch is an error");
        check(inflater.run({}, output).stop == Inflater::Stop::error, "an error stays");
        Inflater truncated;
        const auto progress = truncated.run(std::span(inflate_vectors::text_stream).first(100), output);
        check(progress.stop == Inflater::Stop::input && progress.consumed == 100, "a cut stream asks for more input");
        const std::array<std::uint8_t, 4> not_zlib{0x12, 0x34, 0x56, 0x78};
        Inflater bad;
        check(bad.run(not_zlib, output).stop == Inflater::Stop::error, "bad header");
    }
}

// Guest unsigned division, r0 / r1 -> quotient r0, remainder r1; a zero divisor returns 0xffffffff.
constexpr std::array<std::uint32_t, 19> divide_code{
    0xe3510000, 0x03e00000, 0x012fff1e,              // cmp r1, #0; mvneq r0, #0; bxeq lr
    0xe3a02000, 0xe3a03001,                          // mov r2, #0; mov r3, #1
    0xe3510102, 0x31510000, 0x31a01081, 0x31a03083,  // 1: cmp r1, #0x80000000; cmpcc r1, r0; lslcc r1, #1; lslcc r3, #1
    0x3afffffa,                                      //    bcc 1b
    0xe1500001, 0x20400001, 0x21822003, 0xe1b030a3,  // 2: cmp r0, r1; subcs r0, r0, r1; orrcs r2, r2, r3; lsrs r3, #1
    0x11a010a1, 0x1afffff9,                          //    lsrne r1, #1; bne 2b
    0xe1a01000, 0xe1a00002, 0xe12fff1e};             // mov r1, r0; mov r0, r2; bx lr

struct Guest {
    Runtime runtime;
    ImportDispatcher dispatcher;
    std::vector<elf::Symbol> exports;
    std::vector<std::string> log;
    Guest() {
        runtime.memory.map(code, 0x3000, Permissions::read_write);
        runtime.memory.map(stack - 0x1000, 0x1000, Permissions::read_write);
    }
    void put(std::uint32_t at, std::span<const std::uint32_t> words) {
        for (std::uint32_t word : words) {
            std::array<std::byte, 4> bytes{};
            for (unsigned i = 0; i < 4; ++i) bytes[i] = std::byte((word >> (8 * i)) & 255);
            check(runtime.memory.write(at, bytes), "write guest word");
            at += 4;
        }
    }
    void seal() { runtime.memory.protect(code, 0x3000, Permissions::read_execute); }
    unsigned install(std::map<std::string, NativeSignature> signatures = {}, std::vector<NativeSet> sets = runtime_library_sets()) {
        NativeOptions options;
        options.stack_top = stack;
        options.return_target = sentinel;
        options.scratch = 0x200000;
        options.code_base = code;
        options.code_bytes = 0x3000;
        options.signatures = std::move(signatures);
        options.log = [this](const std::string& line) { log.push_back(line); };
        return install_native_functions(dispatcher, runtime, exports, std::move(sets), options);
    }
    CallOutcome call(std::uint32_t address, std::uint32_t a, std::uint32_t b) {
        const std::array<std::uint32_t, 2> arguments{a, b};
        return dispatcher.call(runtime, address, arguments, stack, sentinel, 100000);
    }
    [[nodiscard]] std::uint32_t r(unsigned index) const { return static_cast<std::uint32_t>(runtime.cpu.registers[index]); }
    [[nodiscard]] bool logged(const std::string& part) const {
        for (const auto& line : log) if (line.find(part) != std::string::npos) return true;
        return false;
    }
};

void replacement() {
    // Found by name, verified against the guest's code, replaced.
    Guest plain, native;
    for (Guest* guest : {&plain, &native}) {
        guest->put(code, divide_code);
        guest->put(code + 0x1000, divide_code);  // unsigned code under the signed name
        guest->seal();
        guest->exports = {{"__aeabi_uidivmod", code, 76, 2, 1}, {"__aeabi_idivmod", code + 0x1000, 76, 2, 1}};
    }
    check(native.install() == 1, "one function replaced");
    check(native.dispatcher.replaces(code), "the verified function is replaced");
    check(!native.dispatcher.replaces(code + 0x1000) && native.logged("left interpreted: __aeabi_idivmod;"),
          "code that disagrees with the host implementation stays interpreted");
    check(native.logged("native functions replaced: 1 __aeabi_uidivmod;") && native.logged(" inflate") && native.log.size() == 1,
          "one summary line names what was replaced and what was not");
    std::mt19937 random(11);
    for (unsigned i = 0; i < 3000; ++i) {
        const std::uint32_t a = random() >> (random() % 32), b = (random() >> (random() % 32)) | (i % 7 == 0 ? 0U : 1U);
        for (const std::uint32_t address : {code, code + 0x1000}) {
            const auto expected = plain.call(address, a, b), got = native.call(address, a, b);
            check(expected.stop == Stop::returned && got.stop == Stop::returned, "both return");
            check(plain.r(0) == native.r(0) && plain.r(1) == native.r(1), "native and interpreted results agree");
            if (address == code && b != 0) check(got.retired == 0, "the replaced function runs no guest instructions");
        }
    }
    native.dispatcher.reset_import_counts();
    check(native.call(code, 100, 7).stop == Stop::returned && native.r(0) == 14 && native.r(1) == 2, "100 / 7");
    check(native.call(code, 5, 0).stop == Stop::returned && native.r(0) == 0xffffffff, "a declined call runs the guest's code");
    check(native.call(code, 9, 4).stop == Stop::returned && native.r(0) == 2 && native.r(1) == 1, "the replacement is back after a declined call");
    const auto& counts = native.dispatcher.import_counts();
    check(counts.at("native:__aeabi_uidivmod") == 2 && counts.at("native:__aeabi_uidivmod (declined)") == 1, "calls are counted");

    // Found by signature when the image does not export the name.
    const std::string signature_text =
        "# comment\n"
        "__aeabi_uidivmod arm 000051e3 0000e003 1eff2f01 0020a0e3 ???????? 020151e3\n";
    std::string why;
    const auto signatures = parse_native_signatures(signature_text, why);
    check(signatures && signatures->size() == 1 && signatures->begin()->second.bytes.size() == 24, "signature file");
    Guest hidden;
    hidden.put(code + 0x800, divide_code);
    hidden.seal();
    check(hidden.install(*signatures) == 1 && hidden.dispatcher.replaces(code + 0x800), "located by signature");
    check(hidden.call(code + 0x800, 1000, 33).stop == Stop::returned && hidden.r(0) == 30 && hidden.r(1) == 10, "replaced after a signature match");
    Guest twice;
    twice.put(code, divide_code);
    twice.put(code + 0x2000, divide_code);
    twice.seal();
    check(twice.install(*signatures) == 0, "an ambiguous signature replaces nothing");
    check(!parse_native_signatures("name arm 0011", why) && !why.empty(), "a short signature is rejected");
    check(!parse_native_signatures("name mips 0011223344556677", why), "an unknown mode is rejected");
    check(!parse_native_signatures("name arm ??11223344556677 88", why), "a wildcard first byte is rejected");
    // Located by an exported name that the signature file supplies.
    const auto by_symbol = parse_native_signatures("__aeabi_uidivmod symbol private_divide\n", why);
    check(by_symbol && by_symbol->at("__aeabi_uidivmod").symbol == "private_divide", "a symbol line");
    Guest renamed;
    renamed.put(code, divide_code);
    renamed.seal();
    renamed.exports = {{"private_divide", code, 76, 2, 1}};
    check(renamed.install(*by_symbol) == 1 && renamed.dispatcher.replaces(code), "located by a name from the signature file");
    check(!parse_native_signatures("name symbol", why) && !parse_native_signatures("name symbol a b", why), "a symbol line takes one name");

    // A Thumb function.
    Guest thumb;
    const std::array<std::uint32_t, 1> thumb_code{0x47703001};  // adds r0, #1; bx lr
    thumb.put(code, thumb_code);
    thumb.seal();
    check(thumb.call(code | 1, 5, 0).stop == Stop::returned && thumb.r(0) == 6, "thumb guest code");
    check(thumb.dispatcher.replace_function(thumb.runtime, code | 1, "plus100", [](ImportCall& call) -> std::optional<std::string> {
        if (call.arg(0) == 7) { call.decline(); return std::nullopt; }
        call.set_result(call.arg(0) + 100);
        return std::nullopt;
    }), "replace a thumb function");
    check(!thumb.dispatcher.replace_function(thumb.runtime, code | 1, "again", [](ImportCall&) -> std::optional<std::string> { return std::nullopt; }),
          "a function is replaced once");
    check(thumb.call(code | 1, 5, 0).stop == Stop::returned && thumb.r(0) == 105, "thumb replacement");
    check(thumb.call(code | 1, 7, 0).stop == Stop::returned && thumb.r(0) == 8, "thumb decline");
    check(thumb.call(code | 1, 1, 0).stop == Stop::returned && thumb.r(0) == 101, "thumb replacement after a decline");
}

// One instruction replaced in the middle of a function; execution goes on with the next instruction.
void instruction_replacement() {
    constexpr std::uint32_t data = stack - 0x800;
    const std::array<std::uint32_t, 3> arm_code{0xe5d10003, 0xe2800001, 0xe12fff1e};  // ldrb r0, [r1, #3]; add r0, #1; bx lr
    const std::array<std::uint32_t, 2> thumb_code{0x300178c8, 0x00004770};             // ldrb r0, [r1, #3]; adds r0, #1; bx lr
    const auto load_set = [](std::uint32_t* seen) {
        NativeSet set;
        set.name = "load";
        NativeSet::Member member;
        member.name = "load";
        set.members.push_back(std::move(member));
        set.instructions["load"] = [seen](std::uint32_t encoding) -> ImportService {
            *seen = encoding;
            if (encoding != 0xe5d10003 && encoding != 0x78c8) return {};
            return [](ImportCall& call) -> std::optional<std::string> {
                std::array<std::byte, 1> byte{};
                auto& cpu = call.runtime().cpu;
                if (!call.runtime().memory.read(static_cast<std::uint32_t>(cpu.registers[1]) + 3, byte)) return "fault";
                cpu.registers[0] = std::to_integer<std::uint32_t>(byte[0]) * 10;
                return std::nullopt;
            };
        };
        return set;
    };
    for (const bool blocks : {true, false}) {
        for (const bool thumb : {false, true}) {
            Guest guest;
            guest.dispatcher.set_block_cache(blocks);
            if (thumb) guest.put(code, thumb_code); else guest.put(code, arm_code);
            guest.seal();
            const std::array<std::byte, 4> bytes{std::byte{0}, std::byte{0}, std::byte{0}, std::byte{7}};
            check(guest.runtime.memory.write(data, bytes), "write data");
            const std::uint32_t entry = code | (thumb ? 1U : 0U);
            check(guest.call(entry, 0, data).stop == Stop::returned && guest.r(0) == 8, "guest load before");
            std::string why;
            const auto signatures = parse_native_signatures(thumb ? "load thumb c8 78 01 30 70 47 00 00" : "load arm 03 00 d1 e5 01 00 80 e2", why);
            check(signatures.has_value(), "instruction signature parses");
            NativeOptions options;
            options.stack_top = stack;
            options.return_target = sentinel;
            options.scratch = 0x200000;
            options.code_base = code;
            options.code_bytes = 0x3000;
            options.signatures = *signatures;
            options.log = [&guest](const std::string& line) { guest.log.push_back(line); };
            std::uint32_t seen = 0;
            std::vector<NativeSet> sets;
            sets.push_back(load_set(&seen));
            check(install_native_functions(guest.dispatcher, guest.runtime, guest.exports, std::move(sets), options) == 1,
                  "instruction replaced");
            check(seen == (thumb ? 0x78c8U : 0xe5d10003U), "the factory sees the instruction's encoding");
            for (int round = 0; round < 3; ++round)
                check(guest.call(entry, 0, data).stop == Stop::returned && guest.r(0) == 71, "replaced load, then the next instruction");
        }
    }
    // An encoding the factory does not take leaves the instruction alone.
    Guest other;
    const std::array<std::uint32_t, 3> other_code{0xe5d10002, 0xe2800001, 0xe12fff1e};  // ldrb r0, [r1, #2]
    other.put(code, other_code);
    other.seal();
    std::string why;
    NativeOptions options;
    options.stack_top = stack;
    options.return_target = sentinel;
    options.scratch = 0x200000;
    options.code_base = code;
    options.code_bytes = 0x3000;
    options.signatures = *parse_native_signatures("load arm 02 00 d1 e5 01 00 80 e2", why);
    options.log = [&other](const std::string& line) { other.log.push_back(line); };
    std::uint32_t seen = 0;
    std::vector<NativeSet> sets;
    sets.push_back(load_set(&seen));
    check(install_native_functions(other.dispatcher, other.runtime, other.exports, std::move(sets), options) == 0 &&
          !other.dispatcher.replaces(code) && other.logged("left interpreted: load"), "a refused encoding stays guest code");
}

// The zlib services on guest memory, without a guest implementation to compare with.
void zlib_services() {
    Runtime runtime;
    ImportDispatcher dispatcher;
    runtime.memory.map(stack - 0x1000, 0x1000, Permissions::read_write);
    runtime.memory.map(0x200000, NativeProbe::scratch_bytes, Permissions::read_write);
    NativeProbe probe(dispatcher, runtime, stack, sentinel, 0x200000, 1000);
    auto sets = runtime_library_sets();
    const auto service = [&](const std::string& name) -> const ImportService& {
        for (const auto& set : sets) for (const auto& member : set.members) if (member.name == name) return member.service;
        throw std::runtime_error("no such member");
    };
    const auto word = [&](std::uint32_t at) {
        std::array<std::byte, 4> bytes{};
        check(runtime.memory.read(at, bytes), "read guest word");
        std::uint32_t value = 0;
        for (unsigned i = 0; i < 4; ++i) value |= std::to_integer<std::uint32_t>(bytes[i]) << (8 * i);
        return value;
    };
    const auto put = [&](std::uint32_t at, std::uint32_t value) {
        std::array<std::byte, 4> bytes{};
        for (unsigned i = 0; i < 4; ++i) bytes[i] = std::byte((value >> (8 * i)) & 255);
        check(runtime.memory.write(at, bytes), "write guest word");
    };
    const auto text = [&](std::uint32_t at, std::size_t size) {
        std::string out(size, '\0');
        check(runtime.memory.read(at, std::as_writable_bytes(std::span(out))), "read guest bytes");
        return out;
    };
    const std::uint32_t source = 0x200000 + 0xffa, length_at = 0x208000, dest = 0x210000 - 5, stream = 0x220000, version = 0x220100;
    const std::string plain = inflate_vectors::text_plain();
    check(runtime.memory.write(source, std::as_bytes(std::span(inflate_vectors::text_stream))), "write stream");
    const auto size = static_cast<std::uint32_t>(inflate_vectors::text_stream.size());

    using Words = std::vector<std::uint32_t>;
    put(length_at, 0x8000);
    auto result = probe.native(service("uncompress"), Words{dest, length_at, source, size});
    check(result && result->r0 == 0 && word(length_at) == plain.size() && text(dest, plain.size()) == plain, "uncompress across pages");
    put(length_at, 100);
    result = probe.native(service("uncompress"), Words{dest, length_at, source, size});
    check(result && result->r0 == static_cast<std::uint32_t>(-5) && word(length_at) == 100, "uncompress without room");
    result = probe.native(service("uncompress"), Words{dest, 0x700000, source, size});
    check(!result, "an inaccessible length stops the guest");
    result = probe.native(service("adler32"), Words{1, source, size});
    check(result && result->r0 == adler32(1, inflate_vectors::text_stream), "adler32 service");
    result = probe.native(service("crc32"), Words{0, 0, 0});
    check(result && result->r0 == 0, "crc32 of nothing");
    result = probe.native(service("__aeabi_idivmod"), Words{static_cast<std::uint32_t>(-7), 2});
    check(result && result->r0 == static_cast<std::uint32_t>(-3) && result->r1 == static_cast<std::uint32_t>(-1), "signed division truncates");
    result = probe.native(service("__aeabi_idiv"), Words{0x80000000, 0xffffffff});
    check(result && result->r0 == 0x80000000, "INT_MIN / -1 wraps");
    check(!probe.native(service("__aeabi_uidiv"), Words{1, 0}), "division by zero is declined");

    // The stream interface, a few bytes at a time.
    const std::array<std::uint8_t, 6> version_text{'1', '.', '2', '.', '3', 0};
    check(runtime.memory.write(version, std::as_bytes(std::span(version_text))), "write version");
    result = probe.native(service("inflateInit_"), Words{stream, version, 40});
    check(result && result->r0 == static_cast<std::uint32_t>(-6), "a different stream structure is refused");
    result = probe.native(service("inflateInit_"), Words{stream, version, 56});
    check(result && result->r0 == 0 && word(stream + 28) != 0, "inflateInit_");
    for (int pass = 0; pass < 2; ++pass) {
        std::uint32_t fed = 0, written = 0, last = 0;
        put(stream + 4, 0);
        put(stream + 16, 0);
        for (unsigned round = 0; round < 10000; ++round) {
            if (word(stream + 4) == 0 && fed < size) {
                const std::uint32_t more = std::min<std::uint32_t>(13, size - fed);
                put(stream + 0, source + fed);
                put(stream + 4, more);
                fed += more;
            }
            if (word(stream + 16) == 0) {
                put(stream + 12, dest + written);
                put(stream + 16, 17);
                written += 17;
            }
            result = probe.native(service("inflate"), Words{stream, 0});
            check(result.has_value(), "inflate runs");
            last = result->r0;
            if (last != 0) break;
        }
        check(last == 1 && word(stream + 20) == plain.size() && word(stream + 8) == size && text(dest, plain.size()) == plain, "inflate in pieces");
        check(word(stream + 48) == adler32(1, bytes_of(plain)), "stream checksum");
        result = probe.native(service("inflateReset"), Words{stream});
        check(result && result->r0 == 0 && word(stream + 20) == 0, "inflateReset");
    }
    result = probe.native(service("inflate"), Words{stream, 0});
    check(result && result->r0 == static_cast<std::uint32_t>(-5), "no input and no progress");
    result = probe.native(service("inflateEnd"), Words{stream});
    check(result && result->r0 == 0 && word(stream + 28) == 0, "inflateEnd");
    result = probe.native(service("inflate"), Words{stream, 0});
    check(result && result->r0 == static_cast<std::uint32_t>(-2), "inflate on a closed stream");
    check(!probe.native(service("inflateSync"), Words{stream}), "unsupported stream calls stop the guest");
}
// The image services on guest memory: PNG filters undo a filter applied here, ETC1 blocks with known colours.
void image_services() {
    Runtime runtime;
    ImportDispatcher dispatcher;
    runtime.memory.map(stack - 0x1000, 0x1000, Permissions::read_write);
    runtime.memory.map(0x200000, NativeProbe::scratch_bytes, Permissions::read_write);
    NativeProbe probe(dispatcher, runtime, stack, sentinel, 0x200000, 1000);
    auto sets = image_library_sets();
    const auto service = [&](const std::string& set_name) -> const ImportService& {
        for (const auto& set : sets) if (set.name == set_name) return set.members.front().service;
        throw std::runtime_error("no such set");
    };
    const auto put = [&](std::uint32_t at, std::span<const std::uint8_t> data) { check(runtime.memory.write(at, std::as_bytes(data)), "write guest bytes"); };
    const auto put32 = [&](std::uint32_t at, std::uint32_t value) {
        const std::array<std::uint8_t, 4> bytes{static_cast<std::uint8_t>(value), static_cast<std::uint8_t>(value >> 8),
                                                static_cast<std::uint8_t>(value >> 16), static_cast<std::uint8_t>(value >> 24)};
        put(at, bytes);
    };
    const auto get = [&](std::uint32_t at, std::size_t size) {
        std::vector<std::uint8_t> out(size);
        check(runtime.memory.read(at, std::as_writable_bytes(std::span(out))), "read guest bytes");
        return out;
    };
    using Words = std::vector<std::uint32_t>;
    const std::uint32_t info = 0x201000, row_at = 0x202ffd, prior_at = 0x206001;
    std::mt19937 random(3);
    for (const std::uint32_t bpp : {1U, 2U, 3U, 4U, 8U})
        for (std::uint32_t filter = 0; filter <= 4; ++filter) {
            const std::uint32_t row_bytes = bpp * 700;  // crosses a page
            std::vector<std::uint8_t> plain(row_bytes), prior(row_bytes), filtered(row_bytes);
            for (auto& byte : plain) byte = static_cast<std::uint8_t>(random());
            for (auto& byte : prior) byte = static_cast<std::uint8_t>(random());
            for (std::uint32_t i = 0; i < row_bytes; ++i) {  // the encoder's side of the PNG specification
                const int a = i >= bpp ? plain[i - bpp] : 0, b = prior[i], c = i >= bpp ? prior[i - bpp] : 0;
                const int p = a + b - c, pa = std::abs(p - a), pb = std::abs(p - b), pc = std::abs(p - c);
                const int predicted = filter == 0 ? 0 : filter == 1 ? a : filter == 2 ? b : filter == 3 ? (a + b) / 2
                                      : pa <= pb && pa <= pc ? a : pb <= pc ? b : c;
                filtered[i] = static_cast<std::uint8_t>(plain[i] - predicted);
            }
            put(row_at, filtered);
            put(prior_at, prior);
            put32(info, 700);
            put32(info + 4, row_bytes);
            put32(info + 8, (bpp * 8) << 24);
            check(probe.native(service("png_read_filter_row"), Words{0x200000, info, row_at, prior_at, filter}).has_value(), "filter service runs");
            check(get(row_at, row_bytes) == plain, "the PNG filter is undone");
            check(get(prior_at, row_bytes) == prior, "the prior row is untouched");
        }
    check(!probe.native(service("png_read_filter_row"), Words{0x200000, info, row_at, prior_at, 5}), "an unknown filter is declined");

    // ETC1: one block per case, 4x4 pixels of R, G, B, 255.
    const std::uint32_t width_at = 0x210000, height_at = 0x210004, mode_at = 0x210008, source_at = 0x211000, dest_at = 0x218000;
    const auto decode = [&](std::span<const std::uint8_t> blocks, std::uint32_t width, std::uint32_t height, std::uint32_t mode = 0) {
        put(source_at, blocks);
        put32(width_at, width);
        put32(height_at, height);
        put32(mode_at, mode);
        const std::vector<std::uint8_t> blank(4096, std::uint8_t{0x5a});
        put(dest_at, blank);
        return probe.native(service("PVRTDecompressETC (RGBA)"), Words{source_at, width_at, height_at, dest_at, mode_at});
    };
    const auto pixel = [&](std::uint32_t index) { return get(dest_at + index * 4, 4); };
    using Pixel = std::vector<std::uint8_t>;
    // Individual mode, left half (0x10, 0x20, 0x30)*17/16... nibbles 1,2,3 and right half nibbles 15,0,8; tables 0 and 7.
    // Pixel indices: pixel (0,0) large positive, pixel (3,3) large negative, everything else small positive.
    const std::array<std::uint8_t, 8> individual{0x1f, 0x20, 0x38, 0x1c, 0x80, 0x00, 0x80, 0x01};
    auto result = decode(individual, 4, 4);
    check(result && result->r0 == 8, "one block is eight source bytes");
    check(pixel(0) == Pixel{17 + 8, 34 + 8, 51 + 8, 255}, "individual mode, large modifier");
    check(pixel(1) == Pixel{17 + 2, 34 + 2, 51 + 2, 255}, "individual mode, small modifier");
    check(pixel(2) == Pixel{255, 47, 136 + 47, 255}, "second half, its own table, clamped high");
    check(pixel(15) == Pixel{255 - 183, 0, 0, 255}, "negative modifier, clamped low");
    // Differential mode, flipped: base 16 (5 bits) -> 132, second base 16 - 3 = 13 -> 107; table 1 for both.
    const std::array<std::uint8_t, 8> differential{0x85, 0x85, 0x85, 0x27, 0x00, 0x00, 0x00, 0x00};
    result = decode(differential, 4, 4);
    check(result && pixel(0) == Pixel{137, 137, 137, 255} && pixel(4) == Pixel{137, 137, 137, 255}, "differential mode, top half");
    check(pixel(8) == Pixel{112, 112, 112, 255} && pixel(15) == Pixel{112, 112, 112, 255}, "flipped block, bottom half");
    result = decode(individual, 2, 2);
    check(result && result->r0 == 8 && pixel(0) == Pixel{25, 42, 59, 255} && pixel(1) == Pixel{19, 36, 53, 255} &&
          pixel(2) == Pixel{19, 36, 53, 255} && pixel(4) == Pixel{0x5a, 0x5a, 0x5a, 0x5a}, "a level smaller than a block is cropped");
    put(source_at, individual);
    put32(mode_at, 0);
    put32(width_at, 4);
    const std::vector<std::uint8_t> blank(64, std::uint8_t{0x5a});
    put(dest_at, blank);
    put32(height_at, 4);
    result = probe.native(service("PVRTDecompressETC (BGRA)"), Words{source_at, width_at, height_at, dest_at, mode_at});
    check(result && pixel(0) == Pixel{59, 42, 25, 255}, "the other byte order");
    check(!decode(individual, 4, 4, 1), "another mode is declined");
    check(!decode(individual, 6, 4), "a size that is not whole blocks is declined");
}
}

// The POD model source: its members are taken only when their code is exactly the known code (branch targets aside).
// The test assembles that code with a byte-copy memcpy, lets the install compare guest and host, and reads through it.
void pod_source() {
    const std::uint32_t read = code + 0x1000, skip = code + 0x1100, read32 = code + 0x1200, marker = code + 0x1300, size = code + 0x1400,
                        data_ptr = code + 0x1408, copy = code + 0x1410;
    const auto branch = [](std::uint32_t at, std::uint32_t target, std::uint32_t top) { return top | (((target - at - 8) >> 2) & 0x00ffffffU); };
    const auto assemble = [&](Guest& guest, std::uint32_t size_word) {
        std::array<std::uint32_t, 24> r{
            0xe92d40f8, 0xe5907008, 0xe1a04000, 0xe5900004, 0xe0827007, 0xe1a06001, 0xe1a05002, 0, 0xe1570000, 0x8a00000b,
            0xe5940004, 0, 0xe5941008, 0xe1a02005, 0xe0801001, 0xe1a00006, 0, 0xe5943008, 0xe3a00001, 0xe0835005,
            0xe5845008, 0xe8bd80f8, 0xe3a00000, 0xe8bd80f8};
        r[7] = branch(read + 28, size, 0xeb000000);
        r[11] = branch(read + 44, data_ptr, 0xeb000000);
        r[16] = branch(read + 64, copy, 0xeb000000);
        std::array<std::uint32_t, 14> k{0xe92d4070, 0xe5906008, 0xe1a04000, 0xe5900004, 0xe1a05001, 0xe0816006, 0, 0xe1560000,
                                         0x95943008, 0x93a00001, 0x83a00000, 0x90835005, 0x95845008, 0xe8bd8070};
        k[6] = branch(skip + 24, size, 0xeb000000);
        const std::array<std::uint32_t, 20> w{
            0xe92d4013, 0xe5903000, 0xe1a04001, 0xe3a02004, 0xe5933008, 0xe28d1004, 0xe12fff33, 0xe3500000, 0x0a000008, 0xe5dd3006,
            0xe5dd2007, 0xe1a03803, 0xe1833c02, 0xe5dd2004, 0xe1833002, 0xe5dd2005, 0xe1833402, 0xe5843000, 0xe28dd008, 0xe8bd8010};
        std::array<std::uint32_t, 11> m{0xe92d4038, 0xe1a04000, 0xe1a05002, 0, 0xe3500000, 0x0a000003, 0xe1a00004, 0xe1a01005,
                                         0xe8bd4038, 0, 0xe8bd8038};
        m[3] = branch(marker + 12, read32, 0xeb000000);
        m[9] = branch(marker + 36, read32, 0xea000000);
        const std::array<std::uint32_t, 12> tail{size_word, 0xe12fff1e, 0xe590000c, 0xe12fff1e,
                                                  // memcpy: cmp r2, #0; bxeq lr; mov r3, r0; 1: ldrb r12, [r1], #1; strb r12, [r3], #1; subs r2, #1; bne 1b; bx lr
                                                  0xe3520000, 0x012fff1e, 0xe1a03000, 0xe4d1c001, 0xe4c3c001, 0xe2522001, 0x1afffffb, 0xe12fff1e};
        guest.put(read, r);
        guest.put(skip, k);
        guest.put(read32, w);
        guest.put(marker, m);
        guest.put(size, tail);
        guest.seal();
        guest.exports = {{"_ZN13CSourceStream4ReadEPvj", read, 96, 2, 1}, {"_ZN13CSourceStream4SkipEj", skip, 56, 2, 1},
                         {"_ZN7CSource6Read32IjEEbRT_", read32, 80, 2, 1}, {"_ZN7CSource10ReadMarkerERjS0_", marker, 44, 2, 1}};
    };
    Guest guest;
    assemble(guest, 0xe5900008);
    check(guest.install({}, image_library_sets()) == 4 && guest.logged("pod_source_read pod_source_skip pod_source_read32 pod_source_marker"),
          "the POD source members are verified and replaced");
    // A stream over 10 bytes in a heap page, read through the replaced members.
    const std::uint32_t heap = 0x300000, file = heap, stream = heap + 0x40, table = heap + 0x80, out = heap + 0xc0, bytes = heap + 0x100;
    guest.runtime.memory.map(heap, 0x1000, Permissions::read_write);
    guest.put(file + 8, std::array<std::uint32_t, 2>{10, bytes});
    guest.put(stream, std::array<std::uint32_t, 3>{table, file, 0});
    guest.put(table + 8, std::array<std::uint32_t, 1>{read});
    guest.put(bytes, std::array<std::uint32_t, 3>{0x44332211, 0x88776655, 0x0000aa99});
    const auto position = [&] {
        std::array<std::byte, 4> word{};
        check(guest.runtime.memory.read(stream + 8, word), "read position");
        return std::to_integer<std::uint32_t>(word[0]);
    };
    const std::array<std::uint32_t, 3> marker_arguments{stream, out, out + 4};
    check(guest.dispatcher.call(guest.runtime, marker, marker_arguments, stack, sentinel, 100000).stop == Stop::returned && guest.r(0) == 1 &&
          position() == 8, "ReadMarker reads two words");
    std::array<std::byte, 8> got{};
    check(guest.runtime.memory.read(out, got) && std::to_integer<std::uint32_t>(got[4]) == 0x55 && std::to_integer<std::uint32_t>(got[7]) == 0x88,
          "the second word");
    guest.put(stream + 8, std::array<std::uint32_t, 1>{0});
    check(guest.call(read32, stream, out).stop == Stop::returned && guest.r(0) == 1 && position() == 4, "Read32 reads four bytes");
    check(guest.runtime.memory.read(out, got) && std::to_integer<std::uint32_t>(got[0]) == 0x11 && std::to_integer<std::uint32_t>(got[3]) == 0x44,
          "little-endian word");
    check(guest.call(skip, stream, 5).stop == Stop::returned && guest.r(0) == 1 && position() == 9, "Skip advances");
    check(guest.call(read32, stream, out).stop == Stop::returned && guest.r(0) == 0 && position() == 9, "a read past the end fails");
    check(guest.call(skip, stream, 2).stop == Stop::returned && guest.r(0) == 0 && position() == 9, "a skip past the end fails");
    check(guest.dispatcher.import_counts().at("native:pod_source_read32") == 2, "served by the host");
    // A getter with another layout (size at 16) is not the known code: the whole set stays interpreted.
    Guest other;
    assemble(other, 0xe5900010);
    check(other.install({}, image_library_sets()) == 0 && other.logged("left interpreted: POD model source"), "other code is left alone");
}

int main() {
    try {
        decoder();
        replacement();
        instruction_replacement();
        zlib_services();
        image_services();
        pod_source();
        std::cout << "native function tests passed: " << checks << " checks\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "native function test failed: " << error.what() << '\n';
        return 1;
    }
}
