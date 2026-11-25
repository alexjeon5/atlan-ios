// SPDX-License-Identifier: GPL-2.0-or-later
// Host implementations of run-time library code that is linked into guest images: the ARM run-time ABI's
// integer division helpers, and zlib's checksums and inflate (written against the public zlib interface and
// RFC 1950/1951; the stream structure below is the documented z_stream of a 32-bit build).
#include "atlan/inflate.hpp"
#include "atlan/inflate_vectors.hpp"
#include "atlan/native_functions.hpp"
#include <cstring>
#include <limits>
#include <memory>
#include <unordered_map>

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
[[nodiscard]] bool store(Memory& memory, std::uint32_t at, std::span<const std::uint8_t> data) {
    return memory.write(at, std::as_bytes(data));
}
[[nodiscard]] std::span<const std::uint8_t> readable(Memory& memory, std::uint32_t at, std::uint32_t limit) {
    const auto page = memory.page_span(at, MemoryAccess::read);
    return {reinterpret_cast<const std::uint8_t*>(page.data()), std::min<std::size_t>(page.size(), limit)};
}
[[nodiscard]] std::span<std::uint8_t> writable(Memory& memory, std::uint32_t at, std::uint32_t limit) {
    const auto page = memory.page_span(at, MemoryAccess::write);
    return {reinterpret_cast<std::uint8_t*>(page.data()), std::min<std::size_t>(page.size(), limit)};
}

// --- Integer division (ARM run-time ABI: quotient in r0, remainder in r1). Division by zero is left to the
// guest's own code, whose behaviour is its platform's choice.
NativeSet division(const char* name, std::vector<std::string> symbols, bool is_signed, bool remainder) {
    NativeSet set;
    set.name = name;
    ImportService service = [is_signed, remainder](ImportCall& call) -> Error {
        const std::uint32_t a = call.arg(0), b = call.arg(1);
        if (b == 0) { call.decline(); return std::nullopt; }
        std::uint32_t quotient, rest;
        if (is_signed) {
            // INT_MIN / -1 wraps, as the shift-and-subtract helpers do.
            const std::int64_t x = static_cast<std::int32_t>(a), y = static_cast<std::int32_t>(b);
            quotient = static_cast<std::uint32_t>(x / y);
            rest = static_cast<std::uint32_t>(x % y);
        } else {
            quotient = a / b;
            rest = a % b;
        }
        call.set_result(quotient);
        if (remainder) call.runtime().cpu.registers[1] = rest;
        return std::nullopt;
    };
    set.members.push_back({name, std::move(symbols), std::move(service), true});
    set.verify = [name = std::string(name), remainder](NativeProbe&, const NativeSet::Call& guest, const NativeSet::Call& native) {
        static constexpr std::uint32_t values[] = {0, 1, 2, 3, 7, 10, 100, 255, 256, 65521, 0x7fffffff, 0x80000000, 0x80000001,
                                                   0xfffffff9, 0xffffffff, 123456789, 0xdeadbeef};
        for (const std::uint32_t a : values)
            for (const std::uint32_t b : values) {
                if (b == 0) continue;
                const Words arguments{a, b};
                const auto g = guest(name, arguments), n = native(name, arguments);
                if (!g || !n || g->r0 != n->r0 || (remainder && g->r1 != n->r1)) return false;
            }
        return true;
    };
    return set;
}

// --- zlib checksums: (value, buffer, length) -> value; a null buffer asks for the initial value.
NativeSet checksum(const char* name, std::uint32_t initial, std::uint32_t (*update)(std::uint32_t, std::span<const std::uint8_t>) noexcept) {
    NativeSet set;
    set.name = name;
    ImportService service = [name = std::string(name), initial, update](ImportCall& call) -> Error {
        std::uint32_t value = call.arg(0), at = call.arg(1), left = call.arg(2);
        if (at == 0) { call.set_result(initial); return std::nullopt; }
        while (left != 0) {
            const auto bytes = readable(call.runtime().memory, at, left);
            if (bytes.empty()) return name + ": the buffer is not readable guest memory";
            value = update(value, bytes);
            at += static_cast<std::uint32_t>(bytes.size());
            left -= static_cast<std::uint32_t>(bytes.size());
        }
        call.set_result(value);
        return std::nullopt;
    };
    set.members.push_back({name, {name}, std::move(service), true});
    set.verify = [name = std::string(name), initial](NativeProbe& probe, const NativeSet::Call& guest, const NativeSet::Call& native) {
        std::vector<std::uint8_t> data(20000);
        std::uint32_t seed = 12345;
        for (auto& byte : data) { seed = seed * 1664525U + 1013904223U; byte = static_cast<std::uint8_t>(seed >> 24); }
        std::fill(data.begin() + 9000, data.begin() + 16000, std::uint8_t{0xff});  // long runs of large bytes
        const std::uint32_t buffer = probe.scratch() + 5;
        if (!store(probe.memory(), buffer, data)) return false;
        const Words cases[] = {{0, 0, 0}, {initial, buffer, 0}, {initial, buffer, 1}, {initial, buffer, 7}, {initial, buffer, 4099},
                               {initial, buffer, 20000}, {0x12345678, buffer + 3, 5553}, {0x7fff0abc, buffer + 9000, 7000}};
        for (const auto& arguments : cases) {
            const auto g = guest(name, arguments), n = native(name, arguments);
            if (!g || !n || g->r0 != n->r0) return false;
        }
        return true;
    };
    return set;
}

// --- zlib inflate. Return values and the z_stream fields are those of zlib.h.
constexpr std::uint32_t z_ok = 0, z_stream_end = 1, z_need_dict = 2, z_stream_error = static_cast<std::uint32_t>(-2),
                        z_data_error = static_cast<std::uint32_t>(-3), z_buf_error = static_cast<std::uint32_t>(-5),
                        z_version_error = static_cast<std::uint32_t>(-6);
constexpr std::uint32_t z_finish = 4;
namespace zs {  // z_stream field offsets, 32-bit
constexpr std::uint32_t next_in = 0, avail_in = 4, total_in = 8, next_out = 12, avail_out = 16, total_out = 20, msg = 24,
                        state = 28, adler = 48, size = 56;
}
struct Pump {
    std::uint32_t next_in{}, avail_in{}, next_out{}, avail_out{};
    std::uint32_t consumed{}, produced{};
    Inflater::Stop stop{};
};
// Feeds guest memory through the decoder page by page. False: a buffer is not accessible guest memory.
[[nodiscard]] bool pump(Inflater& inflater, Memory& memory, Pump& p) {
    while (true) {
        const auto in = p.avail_in != 0 ? readable(memory, p.next_in, p.avail_in) : std::span<const std::uint8_t>{};
        const auto out = p.avail_out != 0 ? writable(memory, p.next_out, p.avail_out) : std::span<std::uint8_t>{};
        if ((p.avail_in != 0 && in.empty()) || (p.avail_out != 0 && out.empty())) return false;
        const auto progress = inflater.run(in, out);
        const auto consumed = static_cast<std::uint32_t>(progress.consumed), produced = static_cast<std::uint32_t>(progress.produced);
        p.next_in += consumed; p.avail_in -= consumed; p.consumed += consumed;
        p.next_out += produced; p.avail_out -= produced; p.produced += produced;
        p.stop = progress.stop;
        if (progress.stop == Inflater::Stop::input && p.avail_in != 0) continue;
        if (progress.stop == Inflater::Stop::output && p.avail_out != 0) continue;
        return true;
    }
}

NativeSet uncompress_set() {
    NativeSet set;
    set.name = "uncompress";
    // int uncompress(dest, destLen*, source, sourceLen)
    ImportService service = [](ImportCall& call) -> Error {
        Memory& memory = call.runtime().memory;
        Pump p;
        p.next_out = call.arg(0);
        p.next_in = call.arg(2);
        p.avail_in = call.arg(3);
        if (!load32(memory, call.arg(1), p.avail_out)) return "uncompress: the length is not readable guest memory";
        Inflater inflater;
        if (!pump(inflater, memory, p)) return "uncompress: a buffer is not accessible guest memory";
        if (p.stop == Inflater::Stop::end) {
            if (!store32(memory, call.arg(1), p.produced)) return "uncompress: the length is not writable guest memory";
            call.set_result(z_ok);
        } else {
            call.set_result(p.stop == Inflater::Stop::error ? z_data_error : p.stop == Inflater::Stop::dictionary ? z_need_dict : z_buf_error);
        }
        return std::nullopt;
    };
    set.members.push_back({"uncompress", {"uncompress"}, std::move(service), true});
    set.verify = [](NativeProbe& probe, const NativeSet::Call& guest, const NativeSet::Call& native) {
        Memory& memory = probe.memory();
        const std::uint32_t source = probe.scratch(), length_at = probe.scratch() + 0x8000, dest = probe.scratch() + 0x10000;
        struct Case { std::span<const std::uint8_t> stream; std::uint32_t source_bytes, room; };
        const auto whole = [](std::span<const std::uint8_t> s, std::uint32_t room) { return Case{s, static_cast<std::uint32_t>(s.size()), room}; };
        const Case cases[] = {whole(inflate_vectors::text_stream, 0x8000), whole(inflate_vectors::hello_stream, 23),
                              whole(inflate_vectors::stored_stream, 0x8000), whole(inflate_vectors::text_stream, 100),
                              {inflate_vectors::text_stream, 100, 0x8000}};
        for (const auto& c : cases) {
            std::vector<std::uint8_t> outputs[2];
            std::optional<NativeProbe::Result> results[2];
            std::uint32_t lengths[2]{};
            for (int side = 0; side < 2; ++side) {
                const std::vector<std::uint8_t> zeros(0x8000);
                if (!store(memory, source, c.stream) || !store(memory, dest, zeros) || !store32(memory, length_at, c.room)) return false;
                const Words arguments{dest, length_at, source, c.source_bytes};
                results[side] = side == 0 ? guest("uncompress", arguments) : native("uncompress", arguments);
                outputs[side].resize(c.room);
                if (!results[side] || !load32(memory, length_at, lengths[side]) ||
                    !memory.read(dest, std::as_writable_bytes(std::span(outputs[side])))) return false;
            }
            if (results[0]->r0 != results[1]->r0 || lengths[0] != lengths[1]) return false;
            if (results[0]->r0 == z_ok && outputs[0] != outputs[1]) return false;
        }
        return true;
    };
    return set;
}

NativeSet inflate_set() {
    NativeSet set;
    set.name = "inflate";
    struct Streams {
        std::unordered_map<std::uint32_t, Inflater> open;  // z_stream.state value -> decoder
        std::uint32_t next{0x5a1f0001};
    };
    const auto streams = std::make_shared<Streams>();
    const auto find = [streams](Memory& memory, std::uint32_t stream) -> Inflater* {
        std::uint32_t handle = 0;
        if (stream == 0 || !load32(memory, stream + zs::state, handle)) return nullptr;
        const auto it = streams->open.find(handle);
        return it == streams->open.end() ? nullptr : &it->second;
    };
    // int inflateInit2_(strm, windowBits, version, stream_size); a negative windowBits means no zlib framing.
    const auto init = [streams](ImportCall& call, std::uint32_t stream, std::int32_t window_bits, std::uint32_t version, std::uint32_t size) -> Error {
        Memory& memory = call.runtime().memory;
        std::array<std::byte, 1> first{};
        if (version == 0 || !memory.read(version, first) || first[0] != std::byte{'1'} || size != zs::size) {
            call.set_result(z_version_error);
            return std::nullopt;
        }
        const bool framed = window_bits >= 0;
        const std::int32_t bits = framed ? window_bits : -window_bits;
        if (stream == 0 || bits < 8 || bits > 15) { call.set_result(z_stream_error); return std::nullopt; }
        const std::uint32_t handle = streams->next++;
        if (!store32(memory, stream + zs::msg, 0) || !store32(memory, stream + zs::state, handle) ||
            !store32(memory, stream + zs::total_in, 0) || !store32(memory, stream + zs::total_out, 0) ||
            !store32(memory, stream + zs::adler, 1)) return "inflateInit: the stream is not writable guest memory";
        streams->open.emplace(handle, Inflater(framed));
        call.set_result(z_ok);
        return std::nullopt;
    };
    set.members.push_back({"inflateInit_", {"inflateInit_"}, [init](ImportCall& call) -> Error {
        return init(call, call.arg(0), 15, call.arg(1), call.arg(2));
    }, true});
    set.members.push_back({"inflateInit2_", {"inflateInit2_"}, [init](ImportCall& call) -> Error {
        return init(call, call.arg(0), static_cast<std::int32_t>(call.arg(1)), call.arg(2), call.arg(3));
    }, true});
    set.members.push_back({"inflateReset", {"inflateReset"}, [find](ImportCall& call) -> Error {
        Memory& memory = call.runtime().memory;
        const std::uint32_t stream = call.arg(0);
        Inflater* inflater = find(memory, stream);
        if (inflater == nullptr) { call.set_result(z_stream_error); return std::nullopt; }
        inflater->reset();
        if (!store32(memory, stream + zs::total_in, 0) || !store32(memory, stream + zs::total_out, 0) ||
            !store32(memory, stream + zs::msg, 0) || !store32(memory, stream + zs::adler, 1)) return "inflateReset: the stream is not writable guest memory";
        call.set_result(z_ok);
        return std::nullopt;
    }, true});
    set.members.push_back({"inflateEnd", {"inflateEnd"}, [streams](ImportCall& call) -> Error {
        Memory& memory = call.runtime().memory;
        const std::uint32_t stream = call.arg(0);
        std::uint32_t handle = 0;
        if (stream == 0 || !load32(memory, stream + zs::state, handle) || streams->open.erase(handle) == 0) {
            call.set_result(z_stream_error);
            return std::nullopt;
        }
        if (!store32(memory, stream + zs::state, 0)) return "inflateEnd: the stream is not writable guest memory";
        call.set_result(z_ok);
        return std::nullopt;
    }, true});
    // int inflate(strm, flush)
    set.members.push_back({"inflate", {"inflate"}, [find](ImportCall& call) -> Error {
        Memory& memory = call.runtime().memory;
        const std::uint32_t stream = call.arg(0);
        Inflater* inflater = find(memory, stream);
        Pump p;
        std::uint32_t total_in = 0, total_out = 0;
        if (inflater == nullptr || !load32(memory, stream + zs::next_in, p.next_in) || !load32(memory, stream + zs::avail_in, p.avail_in) ||
            !load32(memory, stream + zs::next_out, p.next_out) || !load32(memory, stream + zs::avail_out, p.avail_out) ||
            !load32(memory, stream + zs::total_in, total_in) || !load32(memory, stream + zs::total_out, total_out) || p.next_in == 0) {
            call.set_result(z_stream_error);
            return std::nullopt;
        }
        if (!pump(*inflater, memory, p)) return "inflate: a buffer is not accessible guest memory";
        if (!store32(memory, stream + zs::next_in, p.next_in) || !store32(memory, stream + zs::avail_in, p.avail_in) ||
            !store32(memory, stream + zs::next_out, p.next_out) || !store32(memory, stream + zs::avail_out, p.avail_out) ||
            !store32(memory, stream + zs::total_in, total_in + p.consumed) || !store32(memory, stream + zs::total_out, total_out + p.produced) ||
            !store32(memory, stream + zs::adler, inflater->checksum())) return "inflate: the stream is not writable guest memory";
        const bool progress = p.consumed != 0 || p.produced != 0;
        switch (p.stop) {
        case Inflater::Stop::end: call.set_result(z_stream_end); break;
        case Inflater::Stop::error: call.set_result(z_data_error); break;
        case Inflater::Stop::dictionary: call.set_result(z_need_dict); break;
        default: call.set_result(call.arg(1) == z_finish || !progress ? z_buf_error : z_ok);
        }
        return std::nullopt;
    }, true});
    // Calls that reach into the decoder state the guest no longer has. They stop the guest instead of guessing.
    for (const char* name : {"inflateSync", "inflateSetDictionary", "inflateSyncPoint"})
        set.members.push_back({name, {name}, [name = std::string(name)](ImportCall&) -> Error {
            return name + " is not available while inflate is replaced by the host implementation";
        }, false});

    set.verify = [](NativeProbe& probe, const NativeSet::Call& guest, const NativeSet::Call& native) {
        Memory& memory = probe.memory();
        const std::uint32_t stream = probe.scratch(), version = probe.scratch() + 0x100, source = probe.scratch() + 0x1000,
                            dest = probe.scratch() + 0x10000;
        // One decoding session: the stream is fed `in_step` bytes at a time into `out_step` bytes of room. The
        // trace holds what a caller relies on: every return value other than "continue", the final counts, the
        // output.
        const auto session = [&](const NativeSet::Call& call, std::span<const std::uint8_t> data, std::uint32_t in_step,
                                 std::uint32_t out_step, std::uint32_t flush, bool reuse, std::vector<std::uint32_t>& trace) {
            const auto run = [&](const char* name, Words arguments) {
                const auto result = call(name, arguments);
                trace.push_back(result ? result->r0 : 0xbad0bad0U);
                return result.has_value();
            };
            if (!reuse) {
                const std::vector<std::uint8_t> zeros(zs::size);
                const std::uint8_t text[] = {'1', '.', '1', '.', '4', 0};
                if (!store(memory, stream, zeros) || !store(memory, version, text)) return false;
                if (!run("inflateInit_", {stream, version, zs::size})) return false;
            } else if (!run("inflateReset", {stream})) {
                return false;
            }
            if (!store(memory, source, data)) return false;
            std::uint32_t fed = 0, written = 0;
            if (!store32(memory, stream + zs::next_in, source) || !store32(memory, stream + zs::avail_in, 0) ||
                !store32(memory, stream + zs::next_out, dest) || !store32(memory, stream + zs::avail_out, 0)) return false;
            for (unsigned round = 0; round < 4000; ++round) {
                std::uint32_t avail_in = 0, avail_out = 0;
                if (!load32(memory, stream + zs::avail_in, avail_in) || !load32(memory, stream + zs::avail_out, avail_out)) return false;
                if (avail_in == 0 && fed < data.size()) {
                    const std::uint32_t more = std::min<std::uint32_t>(in_step, static_cast<std::uint32_t>(data.size()) - fed);
                    if (!store32(memory, stream + zs::next_in, source + fed) || !store32(memory, stream + zs::avail_in, more)) return false;
                    fed += more;
                }
                if (avail_out == 0) {
                    if (written + out_step > 0x8000) return false;
                    if (!store32(memory, stream + zs::next_out, dest + written) || !store32(memory, stream + zs::avail_out, out_step)) return false;
                    written += out_step;
                }
                if (!run("inflate", {stream, flush})) return false;
                if (trace.back() != z_ok) break;
                trace.pop_back();  // how many calls it takes depends on how far the decoder reads ahead
            }
            for (const std::uint32_t field : {zs::avail_in, zs::total_in, zs::total_out}) {
                std::uint32_t value = 0;
                if (!load32(memory, stream + field, value)) return false;
                trace.push_back(value);
            }
            std::uint32_t total = 0;
            if (!load32(memory, stream + zs::total_out, total) || total > 0x8000) return false;
            std::vector<std::uint8_t> output(total);
            if (!memory.read(dest, std::as_writable_bytes(std::span(output)))) return false;
            trace.insert(trace.end(), output.begin(), output.end());
            return true;
        };
        std::vector<std::uint32_t> traces[2];
        for (int side = 0; side < 2; ++side) {
            const auto& call = side == 0 ? guest : native;
            auto& trace = traces[side];
            const std::vector<std::uint8_t> truncated(inflate_vectors::text_stream.begin(), inflate_vectors::text_stream.begin() + 90);
            if (!session(call, inflate_vectors::text_stream, 37, 50, 0, false, trace) ||
                !session(call, inflate_vectors::hello_stream, 1, 1, 1, true, trace) ||
                !session(call, inflate_vectors::stored_stream, 4096, 4096, 2, true, trace) ||
                !session(call, inflate_vectors::text_stream, 4096, 4096, z_finish, true, trace) ||
                !session(call, truncated, 4096, 4096, 0, true, trace)) return false;
            const auto ended = call("inflateEnd", Words{stream});
            trace.push_back(ended ? ended->r0 : 0xbad0bad0U);
            std::uint32_t state = 1;
            if (!load32(memory, stream + zs::state, state)) return false;
            trace.push_back(state);
        }
        return traces[0] == traces[1];
    };
    return set;
}
}

std::vector<NativeSet> runtime_library_sets() {
    std::vector<NativeSet> sets;
    sets.push_back(division("__aeabi_uidivmod", {"__aeabi_uidivmod"}, false, true));
    sets.push_back(division("__aeabi_idivmod", {"__aeabi_idivmod"}, true, true));
    sets.push_back(division("__aeabi_uidiv", {"__aeabi_uidiv", "__udivsi3"}, false, false));
    sets.push_back(division("__aeabi_idiv", {"__aeabi_idiv", "__divsi3"}, true, false));
    sets.push_back(checksum("adler32", 1, &adler32));
    sets.push_back(checksum("crc32", 0, &crc32));
    sets.push_back(uncompress_set());
    sets.push_back(inflate_set());
    return sets;
}
}
