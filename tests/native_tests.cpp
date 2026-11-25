// SPDX-License-Identifier: GPL-2.0-or-later
// Host replacements of guest functions: the decoder, the services on guest memory, and locating, verifying,
// installing and declining on a synthetic guest.
#include "atlan/inflate.hpp"
#include "atlan/inflate_vectors.hpp"
#include "atlan/native_functions.hpp"
#include <array>
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
    unsigned install(std::map<std::string, NativeSignature> signatures = {}) {
        NativeOptions options;
        options.stack_top = stack;
        options.return_target = sentinel;
        options.scratch = 0x200000;
        options.code_base = code;
        options.code_bytes = 0x3000;
        options.signatures = std::move(signatures);
        options.log = [this](const std::string& line) { log.push_back(line); };
        return install_native_functions(dispatcher, runtime, exports, runtime_library_sets(), options);
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
}

int main() {
    try {
        decoder();
        replacement();
        zlib_services();
        std::cout << "native function tests passed: " << checks << " checks\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "native function test failed: " << error.what() << '\n';
        return 1;
    }
}
