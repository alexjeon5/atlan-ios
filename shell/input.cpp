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
    for (const auto& binding : touch) if (binding) return false;
    return location.empty();
}

std::vector<const NativeBinding*> InputBindings::matching(const InputEvent& event) const {
    std::vector<const NativeBinding*> out;
    if (const auto* t = std::get_if<TouchEvent>(&event)) {
        if (const auto& binding = touch[static_cast<std::size_t>(t->phase)]) out.push_back(&*binding);
    } else {
        for (const auto& binding : location) out.push_back(&binding);
    }
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
bool is_text(InputSource source) { return source == InputSource::gga || source == InputSource::rmc; }

struct SourceName { const char* name; InputSource source; bool touch, location; };
constexpr SourceName sources[] = {
    {"action", InputSource::action, true, false},   {"pointer", InputSource::pointer, true, false},
    {"x", InputSource::x, true, false},             {"y", InputSource::y, true, false},
    {"time", InputSource::time, true, true},        {"latitude", InputSource::latitude, false, true},
    {"longitude", InputSource::longitude, false, true}, {"altitude", InputSource::altitude, false, true},
    {"speed", InputSource::speed, false, true},     {"course", InputSource::course, false, true},
    {"accuracy", InputSource::accuracy, false, true}, {"gga", InputSource::gga, false, true},
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
}

std::string nmea_gga(const LocationFix& fix) {
    const auto t = utc(fix.time_ms);
    char rest[128];
    std::snprintf(rest, sizeof rest, ",1,08,%.1f,%.1f,M,0.0,M,,", std::clamp(fix.accuracy_m / 5.0, 0.5, 50.0), fix.altitude_m);
    return sentence("GPGGA," + clock(t) + "," + angle(fix.latitude, 2, 'N', 'S') + "," + angle(fix.longitude, 3, 'E', 'W') + rest);
}

std::string nmea_rmc(const LocationFix& fix) {
    const auto t = utc(fix.time_ms);
    char speed[64], course[64] = "", date[64];
    std::snprintf(speed, sizeof speed, "%.2f", std::max(0.0, fix.speed_mps) * 3600.0 / 1852.0);  // knots
    if (fix.course_deg >= 0) std::snprintf(course, sizeof course, "%.2f", fix.course_deg);
    std::snprintf(date, sizeof date, "%02d%02d%02d", t.day, t.month, t.year % 100);
    return sentence("GPRMC," + clock(t) + ",A," + angle(fix.latitude, 2, 'N', 'S') + "," + angle(fix.longitude, 3, 'E', 'W') + "," +
                    speed + "," + course + "," + date + ",,,A");
}

std::optional<InputBindings> parse_input_bindings(const std::string& text, std::string& error) {
    InputBindings out;
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
        else if (phase >= 0) out.touch[static_cast<std::size_t>(phase)] = std::move(binding);
        else for (auto& slot : out.touch) slot = binding;
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
    std::uint32_t env, std::uint32_t receiver, std::string& error, const StringArgument& strings) {
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
            const auto handle = strings ? strings(i, source == InputSource::gga ? nmea_gga(*fix) : nmea_rmc(*fix)) : 0;
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
