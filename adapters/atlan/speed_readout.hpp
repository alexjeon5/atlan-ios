// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include "atlan/native_functions.hpp"
#include <atomic>
#include <cstdint>
#include <memory>
#include <optional>
#include <vector>

// Speeds above 255 km/h on the app's own screens.
//
// The app keeps the GPS speed as one byte of whole km/h and saturates it at 255, so every screen stops at 255. Its own
// logic (warnings, voice, map matching) keeps using that byte unchanged. Only the places that show the speed to the
// driver read it through the instructions replaced here: each is a single byte load (A32 LDRB, immediate offset), and
// the host performs the same load; when the byte holds the saturated 255 and the latest fix the host delivered is
// faster, the speed of that fix (in whole km/h, rounded as the app rounds) takes its place.
//
// The loads are located through the embedder's signature file under the member names below; a load that is not found
// or is not such an LDRB is left alone, and that screen keeps showing 255.
namespace atlan::shell::speed {
// Latest delivered fix speed in whole km/h; negative while there is none (no fix yet, or the signal is lost).
using FixSpeed = std::shared_ptr<std::atomic<std::int32_t>>;
[[nodiscard]] FixSpeed make_fix_speed();
// The km/h the app derives from the NMEA speed a fix is sent with (knots to two decimals), or nullopt when the fix
// carries no speed for it.
[[nodiscard]] std::optional<std::int32_t> fix_kmh(double speed_mps, double accuracy_m) noexcept;
// What a replaced load hands the screen: `loaded` itself unless it is the saturated value and `fix` is faster.
[[nodiscard]] std::uint32_t shown_speed(std::uint32_t loaded, std::int32_t fix) noexcept;
// One set per screen. Members: "map_speed_load" (the speed number on the map), "gps_info_speed_load" (the GPS
// information page of the settings).
[[nodiscard]] std::vector<NativeSet> speed_readout_sets(FixSpeed fix);
}
