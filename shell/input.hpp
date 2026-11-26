// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include "shell/render_config.hpp"
#include <array>
#include <cstdint>
#include <functional>
#include <mutex>
#include <optional>
#include <string>
#include <variant>
#include <vector>

// Host input (touch, location) on its way to the guest. The mechanism here is app-neutral: the embedder pushes
// events from its UI thread, the guest thread drains them, and an InputBindings value (the per-app adapter part)
// says which registered native method receives each event and how its arguments are built.
namespace atlan::shell {
enum class TouchPhase : std::uint8_t { down, move, up, cancel };
struct TouchEvent {
    TouchPhase phase{};
    std::uint32_t pointer{};   // stable index of the finger while it is down (0 for the first)
    float x{}, y{};            // guest surface pixels, origin top-left
    std::uint64_t time_ms{};   // monotonic host time
};
struct LocationFix {
    double latitude{}, longitude{};  // WGS84 degrees
    double altitude_m{};
    double speed_mps{};    // negative when unknown
    double course_deg{};   // clockwise from true north; negative when unknown
    double accuracy_m{};   // horizontal accuracy radius; negative when invalid
    std::uint64_t time_ms{};  // Unix epoch milliseconds of the fix
};
using InputEvent = std::variant<TouchEvent, LocationFix>;

// Thread-safe queue from the UI thread to the guest thread. A move replaces a queued move of the same finger when it
// is the newest touch event, and a fix replaces a queued fix when it is the newest event, so a slow guest sees the
// latest position instead of a growing backlog. When full, new moves and fixes are dropped; down/up/cancel always
// enter so a press is never left stuck.
class InputQueue {
public:
    static constexpr std::size_t capacity = 256;
    void push(const TouchEvent& event);
    void push(const LocationFix& fix);
    [[nodiscard]] std::vector<InputEvent> drain();
    [[nodiscard]] std::uint64_t dropped() const;
private:
    mutable std::mutex lock_;
    std::vector<InputEvent> events_;
    std::uint64_t dropped_{};
};

// Notices when location fixes stop coming. After the last real fix, once `silence_ms` has passed without another, lost()
// returns a "no fix" copy of it (accuracy_m = -1, time advanced by the elapsed host time) every `period_ms`, as a GPS
// receiver goes on reporting each second without a position. A fix with a negative accuracy is itself a lost fix.
class LocationWatch {
public:
    static constexpr std::uint64_t silence_ms = 3000, period_ms = 1000;
    void observe(const LocationFix& fix, std::uint64_t now_ms);
    [[nodiscard]] std::optional<LocationFix> lost(std::uint64_t now_ms);
private:
    std::optional<LocationFix> last_;
    std::uint64_t last_ms_{}, next_ms_{};
};

// View point (origin top-left) to guest surface pixels, for a surface shown in `shown` (see fit_rect). Points outside
// `shown` clamp to the surface edge.
struct SurfacePoint { float x{}, y{}; };
[[nodiscard]] SurfacePoint to_surface(double x, double y, ViewRect shown, PixelSize surface) noexcept;

// Where one argument of a bound native method comes from. The value is multiplied by `scale` and then converted to
// the JNI type at that position of the signature (Z B C S I: rounded 32-bit integer, J: rounded 64-bit integer,
// F: float, D: double). The text sources (gga, gsa, gsv, rmc) fill a java/lang/String parameter with one NMEA 0183
// sentence built from the fix (see nmea_gga). gsv is several sentences: its binding is called once per sentence.
enum class InputSource : std::uint8_t {
    constant,       // `scale` itself
    action,         // touch: InputBindings::actions[phase]
    pointer, x, y,  // touch
    time,           // touch: monotonic ms; location: Unix epoch ms
    latitude, longitude, altitude, speed, course, accuracy,  // location
    gga, gsa, gsv, rmc  // location, text
};
struct InputArgument {
    InputSource source{InputSource::constant};
    double scale{1.0};
};
// A registered native method (class, name, JNI signature) and one argument per signature parameter.
struct NativeBinding {
    std::string class_name, name, signature;
    std::vector<InputArgument> arguments;
};
struct InputBindings {
    std::array<std::optional<NativeBinding>, 4> touch;  // indexed by TouchPhase
    std::vector<NativeBinding> location;                // every rule is called for each fix, in order
    std::array<std::int32_t, 4> actions{0, 2, 1, 3};    // value of `action` per TouchPhase (down, move, up, cancel)
    [[nodiscard]] bool empty() const noexcept;
    [[nodiscard]] std::vector<const NativeBinding*> matching(const InputEvent& event) const;
};

// Parses a bindings text. One rule per line, '#' starts a comment:
//   touch[.down|.move|.up|.cancel] CLASS METHOD SIGNATURE ARG...
//   location CLASS METHOD SIGNATURE ARG...
//   actions DOWN MOVE UP CANCEL
// ARG is a source name (action pointer x y time latitude longitude altitude speed course accuracy gga gsa gsv rmc),
// optionally followed by *FACTOR (speed*3.6), or a number (a constant). `touch` without a phase binds all four
// phases; several `location` rules are all called. The argument count must equal the signature's parameter count.
// Parameters must be primitives, or Ljava/lang/String; for a text source.
// On failure returns nullopt and sets `error` to "line N: reason".
[[nodiscard]] std::optional<InputBindings> parse_input_bindings(const std::string& text, std::string& error);
// Reads and parses a bindings file. A missing file is not an error: the result is empty bindings.
[[nodiscard]] std::optional<InputBindings> load_input_bindings(const std::string& path, std::string& error);

// NMEA 0183 sentences for a fix, with checksum and trailing CR LF, as a GPS receiver reports them through Android's
// NMEA listener (one sentence per message). CoreLocation gives no satellite data, so the sky is derived from the
// fix's horizontal accuracy (see sky_for): the smaller the error, the more satellites in use and the stronger their
// signals. All four sentences describe the same sky. HDOP is accuracy / 5 m (0.5 to 50). A fix with an accuracy
// above 100 m reports no position fix (GGA quality 0, GSA fix type 1, RMC status V) with a few weak satellites in
// view; a negative accuracy (signal lost) reports no satellites at all. Without a fix the last position stays in the
// fields. Unknown speed is 0, unknown course is an empty field. Time is the fix time in UTC.
struct SkySatellite { int prn, elevation, azimuth, snr; bool used; };
struct Sky {
    bool fix{};                       // a position fix
    double hdop{};
    std::vector<SkySatellite> view;   // satellites in view, those in use first
    [[nodiscard]] int used() const noexcept;
};
[[nodiscard]] Sky sky_for(const LocationFix& fix);
[[nodiscard]] std::string nmea_gga(const LocationFix& fix);
[[nodiscard]] std::string nmea_gsa(const LocationFix& fix);
[[nodiscard]] std::vector<std::string> nmea_gsv(const LocationFix& fix);
[[nodiscard]] std::string nmea_rmc(const LocationFix& fix);

// How many calls `binding` takes for `event`: the number of sentences of its text argument (gsv), otherwise 1.
[[nodiscard]] std::size_t native_call_count(const NativeBinding& binding, const InputEvent& event);

// Returns the String object handle holding `text` for parameter `index` of the call (0 on failure).
using StringArgument = std::function<std::uint32_t(std::size_t index, const std::string& text)>;
// AAPCS32 (softfp) argument words of the JNI call `binding` for `event`: env, receiver, then the parameters, with
// 64-bit values (J, D) in an even-aligned word pair, low word first. The words go to Runtime::load_arguments as they
// are. `part` (below native_call_count) picks the sentence of a text argument. On failure returns nullopt and sets
// `error`.
[[nodiscard]] std::optional<std::vector<std::uint32_t>> native_call_words(
    const NativeBinding& binding, const InputEvent& event, const std::array<std::int32_t, 4>& actions,
    std::uint32_t env, std::uint32_t receiver, std::string& error, const StringArgument& strings = {},
    std::size_t part = 0);
}
