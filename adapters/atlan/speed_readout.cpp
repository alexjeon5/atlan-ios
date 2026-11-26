// SPDX-License-Identifier: GPL-2.0-or-later
#include "adapters/atlan/speed_readout.hpp"
#include <algorithm>
#include <array>
#include <cmath>

namespace atlan::shell::speed {
namespace {
constexpr std::uint32_t saturated = 255;

// A32 LDRB Rt, [Rn, #+imm12] without writeback, always executed: cond 1110, 010 P=1 U=1 B=1 W=0 L=1.
ImportService byte_load(std::uint32_t encoding, FixSpeed fix) {
    if ((encoding & 0xfff00000U) != 0xe5d00000U) return {};
    const unsigned rn = encoding >> 16 & 15, rt = encoding >> 12 & 15;
    const std::uint32_t offset = encoding & 0xfff;
    if (rn == 15 || rt == 15) return {};
    return [rn, rt, offset, fix = std::move(fix)](ImportCall& call) -> std::optional<std::string> {
        auto& cpu = call.runtime().cpu;
        const std::uint32_t address = static_cast<std::uint32_t>(cpu.registers[rn]) + offset;
        std::array<std::byte, 1> byte{};
        if (!call.runtime().memory.read(address, byte)) return "speed readout: guest byte load faulted";
        cpu.registers[rt] = shown_speed(std::to_integer<std::uint32_t>(byte[0]), fix->load(std::memory_order_relaxed));
        return std::nullopt;
    };
}

NativeSet readout(const char* set, const char* member, const FixSpeed& fix) {
    NativeSet out;
    out.name = set;
    NativeSet::Member load;
    load.name = member;
    out.members.push_back(std::move(load));
    out.instructions[member] = [fix](std::uint32_t encoding) { return byte_load(encoding, fix); };
    return out;
}
}

FixSpeed make_fix_speed() { return std::make_shared<std::atomic<std::int32_t>>(-1); }

std::optional<std::int32_t> fix_kmh(double speed_mps, double accuracy_m) noexcept {
    if (!(accuracy_m >= 0)) return std::nullopt;  // a lost signal: the sentence has no speed
    const double knots = std::round(std::max(0.0, speed_mps) * 3600.0 / 1852.0 * 100.0) / 100.0;
    const double kmh = knots * 1.852 + 0.5;
    return kmh >= 2147483647.0 ? 2147483647 : static_cast<std::int32_t>(kmh);
}

std::uint32_t shown_speed(std::uint32_t loaded, std::int32_t fix) noexcept {
    return loaded == saturated && fix > static_cast<std::int32_t>(saturated) ? static_cast<std::uint32_t>(fix) : loaded;
}

std::vector<NativeSet> speed_readout_sets(FixSpeed fix) {
    return {readout("speed readout (map)", "map_speed_load", fix),
            readout("speed readout (GPS information)", "gps_info_speed_load", fix)};
}
}
