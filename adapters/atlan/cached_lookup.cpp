// SPDX-License-Identifier: GPL-2.0-or-later
#include "adapters/atlan/cached_lookup.hpp"
#include <array>
#include <filesystem>
#include <string>
#include <sys/stat.h>

namespace atlan::shell::cached_lookup {
namespace {
// What a file looked like on the host: absent, or its identity, size and modification time.
struct Stamp {
    bool exists{};
    std::uint64_t device{}, inode{}, size{};
    std::int64_t seconds{}, nanoseconds{};
    friend bool operator==(const Stamp&, const Stamp&) = default;
};
[[nodiscard]] Stamp stamp(const std::filesystem::path& path) {
    struct ::stat st{};
    if (::stat(path.c_str(), &st) != 0) return {};
#if defined(__APPLE__)
    const auto& modified = st.st_mtimespec;
#else
    const auto& modified = st.st_mtim;
#endif
    return {true, static_cast<std::uint64_t>(st.st_dev), static_cast<std::uint64_t>(st.st_ino), static_cast<std::uint64_t>(st.st_size),
            static_cast<std::int64_t>(modified.tv_sec), static_cast<std::int64_t>(modified.tv_nsec)};
}
[[nodiscard]] std::vector<Stamp> stamps(const std::vector<std::filesystem::path>& paths) {
    std::vector<Stamp> out;
    out.reserve(paths.size());
    for (const auto& path : paths) out.push_back(stamp(path));
    return out;
}
}

struct State {
    GuestStdio* files{};
    bool armed{};
    // Taken from the getter's code at install.
    std::uint32_t lookup{}, entry{};
    // The last lookup: the files it resolved, how they looked afterwards, its result, and whether that result holds
    // for as long as the files keep looking so.
    std::vector<std::filesystem::path> paths;
    std::vector<Stamp> after;
    std::uint32_t value{};
    bool kept{};
};

namespace {
[[nodiscard]] bool load_word(Memory& memory, std::uint32_t at, std::uint32_t& out) {
    std::array<std::byte, 4> bytes{};
    if (!memory.read(at, bytes)) return false;
    out = 0;
    for (unsigned k = 0; k < 4; ++k) out |= std::to_integer<std::uint32_t>(bytes[k]) << (8 * k);
    return true;
}
[[nodiscard]] bool store_word(Memory& memory, std::uint32_t at, std::uint32_t value) {
    std::array<std::byte, 4> bytes{};
    for (unsigned k = 0; k < 4; ++k) bytes[k] = std::byte((value >> (8 * k)) & 255U);
    return memory.write(at, bytes);
}

// A32 getter `int get() { int v; lookup(N, &v); return v; }` as the compiler emits it:
//   push {r0, r1, r2, lr}; mov r0, #N; add r1, sp, #4; bl lookup; ldr r0, [sp, #4]; add sp, sp, #12; pop {pc}
// The slot the lookup fills is the pushed r1. Anything else is not this getter.
[[nodiscard]] bool read_getter(Memory& memory, std::uint32_t address, std::uint32_t& lookup, std::uint32_t& entry) {
    if ((address & 3U) != 0) return false;  // Thumb or misaligned
    std::array<std::uint32_t, 7> w{};
    for (unsigned i = 0; i < w.size(); ++i)
        if (!load_word(memory, address + 4 * i, w[i])) return false;
    if (w[0] != 0xe92d4007U || (w[1] & 0xffffff00U) != 0xe3a00000U || w[2] != 0xe28d1004U || (w[3] & 0xff000000U) != 0xeb000000U ||
        w[4] != 0xe59d0004U || w[5] != 0xe28dd00cU || w[6] != 0xe49df004U)
        return false;
    entry = w[1] & 0xffU;
    const auto offset = static_cast<std::int32_t>(w[3] << 8) >> 6;  // imm24 * 4, sign-extended
    lookup = address + 12 + 8 + static_cast<std::uint32_t>(offset);
    return true;
}
}

Shared make_state() { return std::make_shared<State>(); }

void attach(const Shared& state, GuestStdio* files) {
    state->files = files;
    state->armed = false;
    state->kept = false;
    state->paths.clear();
    state->after.clear();
}

void arm(const Shared& state) { state->armed = true; }

std::vector<NativeSet> cached_lookup_sets(Shared state) {
    NativeSet set;
    set.name = "map mode lookup (kept between file changes)";
    NativeSet::Member getter;
    getter.name = "map_mode";
    getter.service = [state](ImportCall& call) -> std::optional<std::string> {
        GuestStdio* files = state->files;
        const bool keep = files != nullptr && state->armed;
        std::vector<Stamp> before;
        if (keep) {
            before = stamps(state->paths);
            if (state->kept && before == state->after) {
                call.set_result(state->value);
                return std::nullopt;
            }
            files->record_paths();
        }
        // The getter's own slot lies in its frame below the caller's stack pointer; callbacks start 256 bytes lower.
        auto& runtime = call.runtime();
        const auto slot = static_cast<std::uint32_t>(runtime.cpu.registers[13]) - 8;
        if (!store_word(runtime.memory, slot, static_cast<std::uint32_t>(runtime.cpu.registers[1])))
            return "map mode lookup: the stack is not writable";
        const std::array<std::uint32_t, 2> arguments{state->entry, slot};
        const auto outcome = call.dispatcher().call_back(runtime, state->lookup, arguments);
        auto paths = keep ? files->take_recorded_paths() : std::vector<std::filesystem::path>{};
        if (outcome.stop != Stop::returned)
            return "map mode lookup did not return normally" + (outcome.message.empty() ? std::string() : ": " + outcome.message);
        std::uint32_t value = 0;
        if (!load_word(runtime.memory, slot, value)) return "map mode lookup: the stack is not readable";
        if (keep) {
            // The result holds while the files look as they did both before and after this lookup (so the lookup read
            // them in that state and changed none of them). A first lookup, or one whose files changed, is kept only
            // after the next lookup confirms it.
            auto after = stamps(paths);
            state->kept = paths == state->paths && after == before;
            state->paths = std::move(paths);
            state->after = std::move(after);
            state->value = value;
        }
        call.set_result(value);
        return std::nullopt;
    };
    set.members.push_back(std::move(getter));
    set.inspect = [state](Memory& memory, const std::map<std::string, std::uint32_t>& located) {
        const auto it = located.find("map_mode");
        return it != located.end() && read_getter(memory, it->second, state->lookup, state->entry);
    };
    // No run-time comparison: before start-up the lookup cannot run yet, and the getter's whole behaviour is the shape
    // checked above.
    return {std::move(set)};
}
}
