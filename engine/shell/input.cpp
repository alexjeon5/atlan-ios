// SPDX-License-Identifier: GPL-2.0-or-later
#include "shell/input.hpp"
#include <algorithm>
#include <bit>
#include <cmath>
#include <cstdio>
#include <ctime>
#include <cstdlib>
#include <fstream>
#include <limits>
#include <sstream>
#include <string_view>

namespace atlan::shell {
void InputQueue::push(const TouchEvent& event) {
    std::lock_guard guard(lock_);
    if (event.phase == TouchPhase::move) {
        // Only the newest touch event may be replaced, or a move would jump ahead of a later down/up.
        for (auto it = events_.rbegin(); it != events_.rend(); ++it) {
            auto* touch = std::get_if<TouchEvent>(&*it);
            if (!touch) continue;
            if (touch->phase == TouchPhase::move && touch->pointer == event.pointer) { *touch = event; return; }
            break;
        }
        if (events_.size() >= capacity) { ++dropped_; return; }
    }
    events_.emplace_back(event);
}

void InputQueue::push(const LocationFix& fix) {
    std::lock_guard guard(lock_);
    if (!events_.empty()) {
        if (auto* queued = std::get_if<LocationFix>(&events_.back())) { *queued = fix; return; }
    }
    if (events_.size() >= capacity) { ++dropped_; return; }
    events_.emplace_back(fix);
}

std::vector<InputEvent> InputQueue::drain() {
    std::lock_guard guard(lock_);
    std::vector<InputEvent> out;
    out.swap(events_);
    return out;
}

std::uint64_t InputQueue::dropped() const {
    std::lock_guard guard(lock_);
    return dropped_;
}

SurfacePoint to_surface(double x, double y, ViewRect shown, PixelSize surface) noexcept {
    const auto axis = [](double value, double origin, double extent, std::uint32_t pixels) {
        if (!(extent > 0) || pixels == 0) return 0.0f;
        const double scaled = (value - origin) / extent * pixels;
        return static_cast<float>(std::clamp(scaled, 0.0, static_cast<double>(pixels - 1)));
    };
    return {axis(x, shown.x, shown.width, surface.width), axis(y, shown.y, shown.height, surface.height)};
}

bool InputBindings::empty() const noexcept {
    for (const auto& rules : touch) if (!rules.empty()) return false;
    return location.empty();
}

std::vector<const NativeBinding*> InputBindings::matching(const InputEvent& event) const {
    std::vector<const NativeBinding*> out;
    if (const auto* t = std::get_if<TouchEvent>(&event)) {
        if (const auto& rules = touch[static_cast<std::size_t>(t->phase)]; !rules.empty()) out.push_back(&rules.front());
    } else {
        for (const auto& binding : location) out.push_back(&binding);
    }
    return out;
}

InputBindings choose_bindings(const InputBindings& all, const std::function<bool(const NativeBinding&)>& fits) {
    InputBindings out;
    out.actions = all.actions;
    for (std::size_t phase = 0; phase < all.touch.size(); ++phase)
        for (const auto& rule : all.touch[phase])
            if (fits(rule)) { out.touch[phase].push_back(rule); break; }
    for (const auto& rule : all.location) if (fits(rule)) out.location.push_back(rule);
    return out;
}

namespace {
// One character per parameter of a JNI signature: the primitive's type character, or 's' for java/lang/String.
// False for any other parameter type.
bool parameters(const std::string& signature, std::string& out, std::string& error) {
    const auto close = signature.find(')');
    if (signature.empty() || signature.front() != '(' || close == std::string::npos || close + 1 >= signature.size()) {
        error = "malformed signature " + signature;
        return false;
    }
    static constexpr std::string_view string_type = "Ljava/lang/String;";
    out.clear();
    for (std::size_t i = 1; i < close;) {
        if (signature.compare(i, string_type.size(), string_type) == 0) { out += 's'; i += string_type.size(); continue; }
        if (std::string_view("ZBCSIJFD").find(signature[i]) == std::string_view::npos) {
            error = "signature " + signature + " has an unsupported parameter type";
            return false;
        }
        out += signature[i++];
    }
    return true;
}
bool is_text(InputSource source) {
    return source == InputSource::gga || source == InputSource::gsa || source == InputSource::gsv || source == InputSource::rmc;
}

struct SourceName { const char* name; InputSource source; bool touch, location; };
constexpr SourceName sources[] = {
    {"action", InputSource::action, true, false},   {"pointer", InputSource::pointer, true, false},
    {"x", InputSource::x, true, false},             {"y", InputSource::y, true, false},
    {"time", InputSource::time, true, true},        {"latitude", InputSource::latitude, false, true},
    {"longitude", InputSource::longitude, false, true}, {"altitude", InputSource::altitude, false, true},
    {"speed", InputSource::speed, false, true},     {"course", InputSource::course, false, true},
    {"accuracy", InputSource::accuracy, false, true}, {"gga", InputSource::gga, false, true},
    {"gsa", InputSource::gsa, false, true},         {"gsv", InputSource::gsv, false, true},
    {"rmc", InputSource::rmc, false, true}};

bool number(const std::string& text, double& out) {
    if (text.empty()) return false;
    char* end{};
    out = std::strtod(text.c_str(), &end);
    return end == text.c_str() + text.size() && std::isfinite(out);
}

bool argument(const std::string& token, bool touch, InputArgument& out, std::string& error) {
    if (number(token, out.scale)) { out.source = InputSource::constant; return true; }
    const auto star = token.find('*');
    const std::string name = token.substr(0, star);
    out.scale = 1.0;
    if (star != std::string::npos && !number(token.substr(star + 1), out.scale)) {
        error = "bad factor in " + token;
        return false;
    }
    for (const auto& s : sources) {
        if (name != s.name) continue;
        if (touch ? !s.touch : !s.location) { error = name + " is not available for this event"; return false; }
        out.source = s.source;
        return true;
    }
    error = "unknown argument " + token;
    return false;
}

double source_value(const InputArgument& a, const InputEvent& event, const std::array<std::int32_t, 4>& actions) {
    if (a.source == InputSource::constant) return a.scale;
    double v = 0;
    if (const auto* t = std::get_if<TouchEvent>(&event)) {
        switch (a.source) {
            case InputSource::action: v = actions[static_cast<std::size_t>(t->phase)]; break;
            case InputSource::pointer: v = t->pointer; break;
            case InputSource::x: v = t->x; break;
            case InputSource::y: v = t->y; break;
            case InputSource::time: v = static_cast<double>(t->time_ms); break;
            default: break;
        }
    } else {
        const auto& f = std::get<LocationFix>(event);
        switch (a.source) {
            case InputSource::time: v = static_cast<double>(f.time_ms); break;
            case InputSource::latitude: v = f.latitude; break;
            case InputSource::longitude: v = f.longitude; break;
            case InputSource::altitude: v = f.altitude_m; break;
            case InputSource::speed: v = f.speed_mps; break;
            case InputSource::course: v = f.course_deg; break;
            case InputSource::accuracy: v = f.accuracy_m; break;
            default: break;
        }
    }
    return v * a.scale;
}

template <typename Integer>
Integer rounded(double value) {
    if (std::isnan(value)) return 0;
    const double r = std::nearbyint(value);
    // The upper bound is exclusive: max()+1 is exactly representable as a double, max() itself may not be.
    if (r < static_cast<double>(std::numeric_limits<Integer>::min())) return std::numeric_limits<Integer>::min();
    if (r >= -static_cast<double>(std::numeric_limits<Integer>::min())) return std::numeric_limits<Integer>::max();
    return static_cast<Integer>(r);
}
}

namespace {
std::string sentence(const std::string& body) {
    unsigned sum = 0;
    for (unsigned char c : body) sum ^= c;
    char tail[16];
    std::snprintf(tail, sizeof tail, "*%02X\r\n", sum);
    return "$" + body + tail;
}
struct UtcTime { int year, month, day, hour, minute; double second; };
UtcTime utc(std::uint64_t time_ms) {
    // Civil date from days since 1970-01-01 (proleptic Gregorian), without the host time zone database.
    const std::uint64_t days = time_ms / 86400000, ms = time_ms % 86400000;
    const std::int64_t z = static_cast<std::int64_t>(days) + 719468;
    const std::int64_t era = z / 146097;
    const std::int64_t doe = z - era * 146097;
    const std::int64_t yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
    const std::int64_t doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
    const std::int64_t mp = (5 * doy + 2) / 153;
    const int day = static_cast<int>(doy - (153 * mp + 2) / 5 + 1);
    const int month = static_cast<int>(mp < 10 ? mp + 3 : mp - 9);
    const int year = static_cast<int>(yoe + era * 400 + (month <= 2 ? 1 : 0));
    return {year, month, day, static_cast<int>(ms / 3600000), static_cast<int>(ms / 60000 % 60), static_cast<double>(ms % 60000) / 1000.0};
}
// "ddmm.mmmm,H" (width 2 for latitude, 3 for longitude)
std::string angle(double degrees, int width, char positive, char negative) {
    const char hemisphere = degrees < 0 ? negative : positive;
    // Round to 1e-4 minutes first so 59.99995 minutes carries into the degrees.
    const auto units = static_cast<std::uint64_t>(std::llround(std::fabs(degrees) * 600000.0));
    char out[64];
    std::snprintf(out, sizeof out, "%0*u%02u.%04u,%c", width, static_cast<unsigned>(units / 600000),
                  static_cast<unsigned>(units % 600000 / 10000), static_cast<unsigned>(units % 10000), hemisphere);
    return out;
}
std::string clock(const UtcTime& t) {
    char out[64];
    const int centis = std::min(5999, static_cast<int>(t.second * 100.0));
    std::snprintf(out, sizeof out, "%02d%02d%02d.%02d", t.hour, t.minute, centis / 100, centis % 100);
    return out;
}

// Satellites a receiver might see, strongest first: PRN, elevation and azimuth in degrees, clear-sky C/N0 in dB-Hz.
struct Orbit { int prn, elevation, azimuth, snr; };
constexpr Orbit orbits[] = {{15, 72, 300, 47}, {2, 62, 40, 46},   {10, 55, 20, 45},  {21, 50, 160, 44}, {5, 45, 110, 43},
                            {26, 35, 280, 42}, {29, 38, 250, 41}, {12, 30, 200, 40}, {18, 25, 330, 38}, {8, 20, 140, 36},
                            {24, 15, 80, 34},  {13, 12, 220, 32}, {31, 8, 60, 28},   {20, 5, 310, 25}};
constexpr int orbit_count = static_cast<int>(sizeof orbits / sizeof orbits[0]);
constexpr double no_fix_accuracy_m = 100.0;
constexpr double no_dop = 99.9;
std::string dop(double value) {
    char out[16];
    std::snprintf(out, sizeof out, "%.1f", value);
    return out;
}
}

int Sky::used() const noexcept {
    return static_cast<int>(std::count_if(view.begin(), view.end(), [](const SkySatellite& s) { return s.used; }));
}

Sky sky_for(const LocationFix& fix) {
    Sky sky;
    const double accuracy = fix.accuracy_m;
    if (!(accuracy >= 0)) { sky.hdop = no_dop; return sky; }  // signal lost: nothing in view
    // A receiver's HDOP for that error. CoreLocation reports a cautious radius (5-16 m on an open road) where a phone's
    // GPS chip shows HDOP 0.7-1.3; receivers rarely go above 2 while they hold a fix. Apps lean on HDOP: above 2 they
    // may stop trusting changes of course, so dividing the radius by a nominal 5 m error made the heading lag in bends.
    sky.hdop = std::clamp(0.5 + accuracy * 0.045, 0.6, 9.9);
    // Satellites in use for a horizontal error: open sky (a few metres) uses about 12, an urban canyon or the edge of a
    // tunnel 3 to 5; beyond 100 m there is no satellite fix at all, only a few weak signals.
    static constexpr std::pair<double, int> in_use[] = {{4, 12}, {6, 11}, {9, 10}, {13, 9}, {18, 8},
                                                        {25, 7}, {35, 6}, {50, 5}, {70, 4}, {no_fix_accuracy_m, 3}};
    int used = 0;
    for (const auto& [limit, count] : in_use) if (accuracy <= limit) { used = count; break; }
    sky.fix = used > 0;
    const int penalty = sky.fix ? std::clamp(static_cast<int>(std::lround((accuracy - 3.0) * 0.5)), 0, 20) : 30;
    const int in_view = sky.fix ? std::min(orbit_count, used + 2) : 4;
    for (int i = 0; i < in_view; ++i) {
        const auto& o = orbits[i];
        const bool in_use_now = i < used;
        sky.view.push_back({o.prn, o.elevation, o.azimuth, std::max(10, o.snr - penalty - (in_use_now ? 0 : 8)), in_use_now});
    }
    return sky;
}

std::string nmea_gga(const LocationFix& fix) {
    const auto t = utc(fix.time_ms);
    const auto sky = sky_for(fix);
    char rest[128];
    std::snprintf(rest, sizeof rest, ",%d,%02d,%s,%.1f,M,0.0,M,,", sky.fix ? 1 : 0, sky.used(), dop(sky.hdop).c_str(), fix.altitude_m);
    return sentence("GPGGA," + clock(t) + "," + angle(fix.latitude, 2, 'N', 'S') + "," + angle(fix.longitude, 3, 'E', 'W') + rest);
}

std::string nmea_gsa(const LocationFix& fix) {
    // Auto mode, fix type (3D or none), the 12 satellite slots, PDOP, HDOP, VDOP and the NMEA 4.1 system id (1: GPS).
    const auto sky = sky_for(fix);
    std::string body = sky.fix ? "GPGSA,A,3" : "GPGSA,A,1";
    char field[32];
    int slots = 0;
    for (const auto& s : sky.view) {
        if (!s.used || slots == 12) continue;
        std::snprintf(field, sizeof field, ",%02d", s.prn);
        body += field;
        ++slots;
    }
    for (; slots < 12; ++slots) body += ",";
    if (sky.fix) body += "," + dop(std::min(no_dop, sky.hdop * 1.5)) + "," + dop(sky.hdop) + "," + dop(std::min(no_dop, sky.hdop * 1.2)) + ",1";
    else body += "," + dop(no_dop) + "," + dop(no_dop) + "," + dop(no_dop) + ",1";
    return sentence(body);
}

std::vector<std::string> nmea_gsv(const LocationFix& fix) {
    constexpr std::size_t per_sentence = 4;
    const auto sky = sky_for(fix);
    const std::size_t count = std::max<std::size_t>(1, (sky.view.size() + per_sentence - 1) / per_sentence);
    std::vector<std::string> out;
    for (std::size_t n = 0; n < count; ++n) {
        char field[64];
        std::snprintf(field, sizeof field, "GPGSV,%zu,%zu,%02zu", count, n + 1, sky.view.size());
        std::string body = field;
        for (std::size_t i = n * per_sentence; i < std::min(sky.view.size(), (n + 1) * per_sentence); ++i) {
            const auto& s = sky.view[i];
            std::snprintf(field, sizeof field, ",%02d,%02d,%03d,%02d", s.prn, s.elevation, s.azimuth, s.snr);
            body += field;
        }
        out.push_back(sentence(body));
    }
    return out;
}

std::string nmea_rmc(const LocationFix& fix) {
    const auto t = utc(fix.time_ms);
    const bool valid = sky_for(fix).fix;
    char speed[64] = "", course[64] = "", date[64];
    if (valid) std::snprintf(speed, sizeof speed, "%.2f", std::max(0.0, fix.speed_mps) * 3600.0 / 1852.0);  // knots
    if (valid && fix.course_deg >= 0) std::snprintf(course, sizeof course, "%.2f", fix.course_deg);
    std::snprintf(date, sizeof date, "%02d%02d%02d", t.day, t.month, t.year % 100);
    return sentence(std::string("GPRMC,") + clock(t) + (valid ? ",A," : ",V,") + angle(fix.latitude, 2, 'N', 'S') + "," +
                    angle(fix.longitude, 3, 'E', 'W') + "," + speed + "," + course + "," + date + ",,," + (valid ? "A" : "N"));
}

void LocationWatch::observe(const LocationFix& fix, std::uint64_t now_ms) {
    if (!(fix.accuracy_m >= 0)) return;  // a lost fix of our own making, or one the embedder reports
    last_ = fix;
    last_ms_ = now_ms;
    next_ms_ = now_ms + silence_ms;
}

std::optional<LocationFix> LocationWatch::lost(std::uint64_t now_ms) {
    if (!last_ || now_ms < next_ms_) return std::nullopt;
    LocationFix out = *last_;
    out.accuracy_m = -1;
    out.speed_mps = -1;
    out.course_deg = -1;
    out.time_ms += now_ms - last_ms_;
    next_ms_ += period_ms;
    if (next_ms_ <= now_ms) next_ms_ = now_ms + period_ms;  // a long stall does not bring a burst
    return out;
}

namespace {
std::vector<std::string> sentences(InputSource source, const LocationFix& fix) {
    switch (source) {
        case InputSource::gga: return {nmea_gga(fix)};
        case InputSource::gsa: return {nmea_gsa(fix)};
        case InputSource::gsv: return nmea_gsv(fix);
        default: return {nmea_rmc(fix)};
    }
}
}

std::size_t native_call_count(const NativeBinding& binding, const InputEvent& event) {
    const auto* fix = std::get_if<LocationFix>(&event);
    std::size_t count = 1;
    if (!fix) return count;
    for (const auto& a : binding.arguments)
        if (is_text(a.source)) count = std::max(count, sentences(a.source, *fix).size());
    return count;
}

std::optional<InputBindings> parse_input_bindings(const std::string& text, std::string& error) {
    InputBindings out;
    std::array<bool, 4> from_plain_touch{};  // the phase's rules come from plain `touch` lines
    std::istringstream lines(text);
    std::string line;
    for (unsigned n = 1; std::getline(lines, line); ++n) {
        const auto fail = [&](const std::string& why) { error = "line " + std::to_string(n) + ": " + why; return std::nullopt; };
        if (const auto hash = line.find('#'); hash != std::string::npos) line.resize(hash);
        std::istringstream words(line);
        std::vector<std::string> w;
        for (std::string word; words >> word;) w.push_back(word);
        if (w.empty()) continue;
        if (w[0] == "actions") {
            if (w.size() != 5) return fail("actions needs DOWN MOVE UP CANCEL");
            for (unsigned i = 0; i < 4; ++i) {
                double v{};
                if (!number(w[i + 1], v) || v != std::nearbyint(v) || std::fabs(v) > 0x7fffffff) return fail("bad action value " + w[i + 1]);
                out.actions[i] = static_cast<std::int32_t>(v);
            }
            continue;
        }
        static constexpr const char* phases[] = {"touch.down", "touch.move", "touch.up", "touch.cancel"};
        int phase = -1;
        for (int i = 0; i < 4; ++i) if (w[0] == phases[i]) phase = i;
        const bool touch = phase >= 0 || w[0] == "touch";
        if (!touch && w[0] != "location") return fail("unknown rule " + w[0]);
        if (w.size() < 4) return fail(w[0] + " needs CLASS METHOD SIGNATURE");
        NativeBinding binding{w[1], w[2], w[3], {}};
        std::string types, why;
        if (!parameters(binding.signature, types, why)) return fail(why);
        if (w.size() - 4 != types.size())
            return fail(binding.signature + " takes " + std::to_string(types.size()) + " arguments, " + std::to_string(w.size() - 4) + " given");
        for (std::size_t i = 4; i < w.size(); ++i) {
            InputArgument a;
            if (!argument(w[i], touch, a, why)) return fail(why);
            if (is_text(a.source) != (types[i - 4] == 's'))
                return fail(w[i] + (is_text(a.source) ? " needs a Ljava/lang/String; parameter" : " cannot fill a String parameter"));
            binding.arguments.push_back(a);
        }
        if (!touch) out.location.push_back(std::move(binding));
        else if (phase >= 0) {
            auto& rules = out.touch[static_cast<std::size_t>(phase)];
            if (from_plain_touch[static_cast<std::size_t>(phase)]) rules.clear();  // the phase's own rule replaces plain ones
            from_plain_touch[static_cast<std::size_t>(phase)] = false;
            rules.push_back(std::move(binding));
        } else {
            for (std::size_t i = 0; i < out.touch.size(); ++i)
                if (from_plain_touch[i] || out.touch[i].empty()) { out.touch[i].push_back(binding); from_plain_touch[i] = true; }
        }
    }
    return out;
}

std::optional<InputBindings> load_input_bindings(const std::string& path, std::string& error) {
    std::ifstream in(path, std::ios::binary);
    if (!in) return InputBindings{};
    std::ostringstream text;
    text << in.rdbuf();
    return parse_input_bindings(text.str(), error);
}

std::optional<std::vector<std::uint32_t>> native_call_words(
    const NativeBinding& binding, const InputEvent& event, const std::array<std::int32_t, 4>& actions,
    std::uint32_t env, std::uint32_t receiver, std::string& error, const StringArgument& strings, std::size_t part) {
    std::string types;
    if (!parameters(binding.signature, types, error)) return std::nullopt;
    if (types.size() != binding.arguments.size()) {
        error = binding.name + binding.signature + " has " + std::to_string(binding.arguments.size()) + " bound arguments";
        return std::nullopt;
    }
    std::vector<std::uint32_t> words{env, receiver};
    const auto wide = [&words](std::uint64_t bits) {
        if (words.size() % 2 != 0) words.push_back(0);  // AAPCS: a 64-bit value starts at an even word
        words.push_back(static_cast<std::uint32_t>(bits));
        words.push_back(static_cast<std::uint32_t>(bits >> 32));
    };
    for (std::size_t i = 0; i < types.size(); ++i) {
        const auto source = binding.arguments[i].source;
        if (is_text(source) || types[i] == 's') {
            const auto* fix = std::get_if<LocationFix>(&event);
            if (!is_text(source) || types[i] != 's' || !fix) { error = binding.name + binding.signature + " text argument mismatch"; return std::nullopt; }
            const auto texts = sentences(source, *fix);
            const auto handle = strings ? strings(i, texts[std::min(part, texts.size() - 1)]) : 0;
            if (handle == 0) { error = binding.name + binding.signature + " cannot create its String argument"; return std::nullopt; }
            words.push_back(handle);
            continue;
        }
        const double v = source_value(binding.arguments[i], event, actions);
        switch (types[i]) {
            case 'D': wide(std::bit_cast<std::uint64_t>(v)); break;
            case 'J': wide(static_cast<std::uint64_t>(rounded<std::int64_t>(v))); break;
            case 'F': words.push_back(std::bit_cast<std::uint32_t>(static_cast<float>(v))); break;
            default: words.push_back(static_cast<std::uint32_t>(rounded<std::int32_t>(v))); break;
        }
    }
    return words;
}
}
