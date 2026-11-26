// SPDX-License-Identifier: GPL-2.0-or-later
// Host input bridge: queue coalescing, binding text, AAPCS32 argument words, and delivery into a synthetic guest
// function that records the words it received. No app-specific names or values.
#include "atlan/imports.hpp"
#include "shell/input.hpp"
#include <algorithm>
#include <array>
#include <bit>
#include <iostream>
#include <stdexcept>

namespace {
using namespace atlan::shell;
unsigned checks{};
void check(bool result, const char* message) {
    ++checks;
    if (!result) throw std::runtime_error(message);
}

void queue() {
    InputQueue q;
    q.push(TouchEvent{TouchPhase::down, 0, 1, 1, 10});
    q.push(TouchEvent{TouchPhase::move, 0, 2, 2, 11});
    q.push(TouchEvent{TouchPhase::move, 0, 3, 3, 12});      // replaces the queued move
    q.push(LocationFix{37.0, 127.0, 0, 0, 0, 5, 1000});
    q.push(LocationFix{37.5, 127.5, 0, 0, 0, 5, 2000});     // replaces the queued fix
    q.push(TouchEvent{TouchPhase::move, 0, 4, 4, 13});      // newest touch event is a move of this finger: replaced
    q.push(TouchEvent{TouchPhase::move, 1, 9, 9, 14});      // another finger: kept
    q.push(TouchEvent{TouchPhase::up, 0, 4, 4, 15});
    q.push(TouchEvent{TouchPhase::move, 1, 8, 8, 16});      // newest touch event is an up: not merged across it
    const auto events = q.drain();
    check(events.size() == 6, "moves and fixes coalesce");
    check(std::get<TouchEvent>(events[0]).phase == TouchPhase::down, "down is kept first");
    check(std::get<TouchEvent>(events[1]).x == 4 && std::get<TouchEvent>(events[1]).time_ms == 13, "latest move wins");
    check(std::get<LocationFix>(events[2]).latitude == 37.5, "latest fix wins");
    check(std::get<TouchEvent>(events[3]).pointer == 1 && std::get<TouchEvent>(events[4]).phase == TouchPhase::up &&
          std::get<TouchEvent>(events[5]).x == 8, "order across fingers and phases is preserved");
    check(q.drain().empty(), "drain empties the queue");

    for (unsigned i = 0; i < InputQueue::capacity + 10; ++i) q.push(TouchEvent{TouchPhase::move, i, 0, 0, i});
    check(q.dropped() == 10, "moves past capacity are dropped and counted");
    q.push(LocationFix{});
    check(q.dropped() == 11, "a fix past capacity is dropped");
    q.push(TouchEvent{TouchPhase::up, 0, 0, 0, 0});
    check(q.drain().size() == InputQueue::capacity + 1, "an up always enters");

    using atlan::shell::fit_rect;
    const auto p = to_surface(426, 196.5, fit_rect(852, 393, {1040, 480}), {1040, 480});
    check(p.x == 520 && p.y == 240, "a surface that fills the view scales both axes");
    const auto boxed = fit_rect(852, 393, {800, 480});  // 655 x 393 centred, 98.5 on each side
    check(boxed.width == 655 && boxed.height == 393 && boxed.x == 98.5 && boxed.y == 0, "a narrower surface is centred with its aspect kept");
    const auto q1 = to_surface(98.5 + 655.0 / 4, 393.0 / 4, boxed, {800, 480});
    check(q1.x == 200 && q1.y == 120, "points map through the shown rectangle");
    const auto edge = to_surface(900, -5, boxed, {800, 480});
    check(edge.x == 799 && edge.y == 0, "points outside the shown rectangle clamp to the surface");
    check(to_surface(10, 10, boxed, {800, 480}).x == 0, "the bar beside the surface clamps to its edge");
    const auto tall = fit_rect(400, 800, {800, 480});
    check(tall.width == 400 && tall.height == 240 && tall.y == 280, "a wider surface is centred vertically");
    check(fit_rect(852, 393, {1040, 480}).width == 852 && fit_rect(852, 393, {1040, 480}).x == 0, "a mismatch under 1% fills the view");
    check(fit_rect(852, 393, {0, 0}).width == 852, "an unknown surface covers the view");
    check(to_surface(1, 1, {0, 0, 0, 0}, {800, 480}).x == 0 && to_surface(1, 1, {0, 0, 10, 10}, {0, 0}).y == 0, "unknown sizes map to the origin");
}

void bindings_text() {
    std::string error;
    const auto parsed = parse_input_bindings(
        "# synthetic adapter\n"
        "touch example/Input onTouch (IFFI)V action x y pointer\n"
        "touch.up example/Input onRelease (I)V pointer   # overrides one phase\n"
        "location example/Input onFix (DDFJI)V latitude longitude speed*3.6 time 7\n"
        "location example/Input onSentence (Ljava/lang/String;)V rmc\n"
        "location example/Input onSentence (Ljava/lang/String;)V gsv\n"
        "actions 10 12 11 13\n", error);
    check(parsed.has_value() && error.empty(), "bindings text parses");
    check(!parsed->empty() && parsed->touch[0].front().name == "onTouch" && parsed->touch[1].front().name == "onTouch" &&
          parsed->touch[2].size() == 1 && parsed->touch[2].front().name == "onRelease" &&
          parsed->touch[3].front().arguments.size() == 4, "touch rules bind phases");
    check(parsed->location.size() == 3 && parsed->location[0].arguments[2].source == InputSource::speed &&
          parsed->location[0].arguments[2].scale == 3.6 && parsed->location[0].arguments[4].source == InputSource::constant &&
          parsed->location[0].arguments[4].scale == 7, "factors and constants parse");
    check(parsed->location[1].name == "onSentence" && parsed->location[1].arguments[0].source == InputSource::rmc &&
          parsed->location[2].arguments[0].source == InputSource::gsv, "text rules parse");
    check(parsed->actions == std::array<std::int32_t, 4>{10, 12, 11, 13}, "action codes parse");
    check(parsed->matching(InputEvent{LocationFix{}}).size() == 3 &&
          parsed->matching(InputEvent{TouchEvent{TouchPhase::up, 0, 0, 0, 0}}).front()->name == "onRelease", "events find their bindings");
    check(InputBindings{}.empty() && InputBindings{}.matching(InputEvent{LocationFix{}}).empty(), "empty bindings bind nothing");

    // Two builds of one app export the same touch native with different parameters: both rules are kept as
    // alternatives, and choose_bindings() keeps the first one the running build declares.
    const auto builds = parse_input_bindings(
        "touch example/Input onTouch (IIJFF)I 1 action time x y\n"
        "touch example/Input onTouch (IJFF)V action time x y\n"
        "touch.up example/Input onRelease (I)V pointer\n"
        "location example/Input onFix (D)V latitude\n"
        "location example/Input onFix (F)V latitude\n", error);
    check(builds && builds->touch[0].size() == 2 && builds->touch[2].size() == 1 && builds->location.size() == 2,
          "touch rules of the same form are alternatives");
    check(builds->matching(InputEvent{TouchEvent{}}).size() == 1 &&
          builds->matching(InputEvent{TouchEvent{}}).front()->signature == "(IIJFF)I", "without a choice the first alternative is called");
    const auto newer = choose_bindings(*builds, [](const NativeBinding& b) { return b.signature != "(IIJFF)I" && b.signature != "(F)V"; });
    check(newer.touch[0].size() == 1 && newer.touch[0].front().signature == "(IJFF)V" && newer.touch[3].front().signature == "(IJFF)V" &&
          newer.touch[2].front().name == "onRelease", "the declared alternative is chosen per phase");
    check(newer.location.size() == 1 && newer.location[0].signature == "(D)V", "location rules the app does not declare are dropped");
    const auto older = choose_bindings(*builds, [](const NativeBinding&) { return true; });
    check(older.touch[1].size() == 1 && older.touch[1].front().signature == "(IIJFF)I", "the first fitting alternative wins");
    check(choose_bindings(*builds, [](const NativeBinding&) { return false; }).empty(), "a phase without a fitting rule binds nothing");

    const auto rejected = [&](const char* text, const char* fragment) {
        error.clear();
        return !parse_input_bindings(text, error) && error.find(fragment) != std::string::npos;
    };
    check(rejected("touch a/B m (II)V x\n", "line 1: (II)V takes 2 arguments, 1 given"), "argument count must match the signature");
    check(rejected("\nlocation a/B m (Ljava/lang/String;)V latitude\n", "line 2: latitude cannot fill a String"), "a number cannot fill a String");
    check(rejected("location a/B m (I)V gga\n", "needs a Ljava/lang/String; parameter"), "a sentence needs a String parameter");
    check(rejected("location a/B m ([B)V gga\n", "unsupported parameter type"), "other reference parameters are rejected");
    check(rejected("touch a/B m (I)V latitude\n", "not available"), "a location source is rejected for touch");
    check(rejected("location a/B m (I)V x\n", "not available"), "a touch source is rejected for location");
    check(rejected("touch a/B m (I)V speed*fast\n", "bad factor") || rejected("touch a/B m (I)V x*fast\n", "bad factor"), "bad factor is rejected");
    check(rejected("gesture a/B m ()V\n", "unknown rule"), "unknown rule is rejected");
    check(rejected("touch a/B m II x y\n", "malformed signature"), "malformed signature is rejected");
    check(rejected("actions 0 1 2\n", "actions needs"), "short action list is rejected");
    check(rejected("touch a/B m (I)V wheel\n", "unknown argument"), "unknown source is rejected");
    const auto missing = load_input_bindings("/nonexistent/atlan-input-bindings", error);
    check(missing.has_value() && missing->empty(), "a missing bindings file means no bindings");
}

void put32(atlan::Memory& memory, std::uint32_t at, std::uint32_t value) {
    std::array<std::byte, 4> bytes{};
    for (unsigned i = 0; i < 4; ++i) bytes[i] = std::byte((value >> (8 * i)) & 255);
    check(memory.write(at, bytes), "write guest word");
}
std::uint32_t get32(atlan::Memory& memory, std::uint32_t at) {
    std::array<std::byte, 4> bytes{};
    check(memory.read(at, bytes), "read guest word");
    std::uint32_t value{};
    for (unsigned i = 0; i < 4; ++i) value |= std::to_integer<std::uint32_t>(bytes[i]) << (i * 8);
    return value;
}

// A guest function that stores r0-r3 and the first four stack words at `record`, then counts the call at record+32.
constexpr std::uint32_t code = 0x1000, record = 0x4000, stack_top = 0x80000, sentinel = 0xffff0000;
struct Guest {
    atlan::Runtime runtime;
    atlan::ImportDispatcher dispatcher;
    Guest() {
        runtime.memory.map(code, 0x1000, atlan::Permissions::read_write);
        runtime.memory.map(record, 0x1000, atlan::Permissions::read_write);
        runtime.memory.map(stack_top - 0x1000, 0x2000, atlan::Permissions::read_write);  // a page above for [sp+12]
        const std::uint32_t recorder[] = {
            0xe59fc03c,                                      // ldr r12, =record
            0xe58c0000, 0xe58c1004, 0xe58c2008, 0xe58c300c,  // str r0-r3, [r12, #0..12]
            0xe59d0000, 0xe58c0010, 0xe59d0004, 0xe58c0014,  // [sp], [sp+4] -> [r12, #16], [r12, #20]
            0xe59d0008, 0xe58c0018, 0xe59d000c, 0xe58c001c,  // [sp+8], [sp+12] -> [r12, #24], [r12, #28]
            0xe59c0020, 0xe2800001, 0xe58c0020,              // ++[r12, #32]
            0xe12fff1e, record};                             // bx lr; literal
        for (unsigned i = 0; i < std::size(recorder); ++i) put32(runtime.memory, code + 4 * i, recorder[i]);
        runtime.memory.protect(code, 0x1000, atlan::Permissions::read_execute);
    }
    std::uint32_t word(unsigned i) { return get32(runtime.memory, record + 4 * i); }
    void deliver(const InputBindings& bindings, const InputEvent& event) {
        const auto matches = bindings.matching(event);
        check(matches.size() == 1, "event has one binding");
        const auto* binding = matches.front();
        std::string error;
        const auto words = native_call_words(*binding, event, bindings.actions, 0xe0e0e0e0, 0x0b0b0b0b, error);
        check(words.has_value(), "argument words are built");
        const auto outcome = dispatcher.call(runtime, code, *words, stack_top, sentinel, 1000);
        check(outcome.stop == atlan::Stop::returned, "guest input handler returns");
    }
};

void nmea() {
    const LocationFix fix{37.5665, 126.978, 38.26, 10, 271.6, 5, 1700000000123ULL};  // 2023-11-14 22:13:20.12 UTC
    const auto valid = [](const std::string& s) {
        const auto star = s.rfind('*');
        if (s.size() < 6 || s[0] != '$' || star == std::string::npos || s.substr(star + 3) != "\r\n") return false;
        unsigned sum = 0;
        for (std::size_t i = 1; i < star; ++i) sum ^= static_cast<unsigned char>(s[i]);
        return std::stoul(s.substr(star + 1, 2), nullptr, 16) == sum;
    };
    const auto gga = nmea_gga(fix), rmc = nmea_rmc(fix);
    check(valid(gga) && valid(rmc), "sentences carry a correct checksum and CR LF");
    check(gga.starts_with("$GPGGA,221320.12,3733.9900,N,12658.6800,E,1,11,0.7,38.3,M,0.0,M,,*"), "GGA fields: 5 m uses 11 satellites");
    check(rmc.starts_with("$GPRMC,221320.12,A,3733.9900,N,12658.6800,E,19.44,271.60,141123,,,A*"), "RMC fields");
    const LocationFix south{-33.999999999, -0.5, 0, -1, -1, 50, 951782400000ULL};  // 2000-02-29 00:00:00 UTC
    check(nmea_gga(south).starts_with("$GPGGA,000000.00,3400.0000,S,00030.0000,W,1,05,2.8,0.0,M"), "minutes carry into degrees; hemispheres; 50 m uses 5");
    check(nmea_rmc(south).starts_with("$GPRMC,000000.00,A,3400.0000,S,00030.0000,W,0.00,,290200,,,A*"), "unknown speed and course; leap day");

    // The sky follows the accuracy: fewer satellites and weaker signals as the error grows.
    const auto sky_at = [&](double accuracy) { LocationFix f = fix; f.accuracy_m = accuracy; return sky_for(f); };
    int previous = 99;
    for (double accuracy : {2.0, 5.0, 8.0, 12.0, 17.0, 24.0, 34.0, 49.0, 69.0, 99.0}) {
        const auto sky = sky_at(accuracy);
        check(sky.fix && sky.used() >= 3 && sky.used() < previous && sky.view.size() == static_cast<std::size_t>(sky.used() + 2),
              "a fix keeps at least 3 satellites in use, fewer as the error grows");
        previous = sky.used();
    }
    check(sky_at(3).view[0].snr > sky_at(40).view[0].snr, "signals weaken as the error grows");
    check(sky_at(5).hdop < 1.0 && sky_at(16).hdop < 1.5 && sky_at(33).hdop <= 2.0 && sky_at(100).hdop <= 5.0,
          "HDOP stays where a receiver holding a fix reports it");
    const auto weak = sky_at(150);
    check(!weak.fix && weak.used() == 0 && weak.view.size() == 4, "beyond 100 m: no fix, a few weak satellites in view");
    const auto lost = sky_at(-1);
    check(!lost.fix && lost.view.empty() && lost.hdop == 99.9, "a negative accuracy is a lost signal");

    // GSA, GSV and GGA agree on the satellites.
    const auto gsa = nmea_gsa(fix);
    check(valid(gsa) && gsa.starts_with("$GPGSA,A,3,15,02,10,21,05,26,29,12,18,08,24,,1.1,0.7,0.9,1*"), "GSA lists the satellites in use");
    const auto fields = [](const std::string& s) { return 1 + std::count(s.begin(), s.end(), ','); };
    check(fields(gsa) == 19, "GSA has 18 fields after its name");
    const auto gsv = nmea_gsv(fix);
    check(gsv.size() == 4 && valid(gsv[0]) && valid(gsv[3]), "13 satellites in view take four GSV sentences");
    check(gsv[0].starts_with("$GPGSV,4,1,13,15,72,300,46,02,62,040,45,10,55,020,44,21,50,160,43*"), "first GSV sentence");
    check(gsv[3].starts_with("$GPGSV,4,4,13,31,08,060,19*"), "last GSV sentence holds the rest; unused satellites are weaker");

    LocationFix far = fix;
    far.accuracy_m = 150;
    check(nmea_gga(far).find(",E,0,00,7.2,") != std::string::npos, "no fix: GGA quality 0 and no satellites in use");
    check(nmea_gsa(far).starts_with("$GPGSA,A,1,,,,,,,,,,,,,99.9,99.9,99.9,1*"), "no fix: GSA fix type 1");
    check(nmea_rmc(far).starts_with("$GPRMC,221320.12,V,3733.9900,N,12658.6800,E,,,141123,,,N*"), "no fix: RMC status V keeps the last position");
    LocationFix gone = fix;
    gone.accuracy_m = -1;
    check(nmea_gsv(gone) == std::vector<std::string>{nmea_gsv(gone)[0]} && nmea_gsv(gone)[0].starts_with("$GPGSV,1,1,00*"), "lost: one empty GSV sentence");
    check(nmea_gga(gone).find(",E,0,00,99.9,") != std::string::npos, "lost: GGA without satellites");
}

void location_watch() {
    LocationWatch watch;
    check(!watch.lost(100000), "nothing to report before the first fix");
    const LocationFix fix{37.5, 127.0, 30, 10, 90, 5, 1700000000000ULL};
    watch.observe(fix, 1000);
    check(!watch.lost(3999), "quiet for less than 3 s is not a loss");
    const auto first = watch.lost(4000);
    check(first && first->accuracy_m < 0 && first->latitude == 37.5 && first->time_ms == 1700000003000ULL && first->speed_mps < 0,
          "after 3 s the last position is reported without a fix, its time moving on");
    check(!watch.lost(4500) && watch.lost(5000) && !watch.lost(5999) && watch.lost(6000), "then once a second");
    check(watch.lost(20000) && !watch.lost(20999) && watch.lost(21000), "a long stall brings one report, not a burst");
    watch.observe(*first, 21500);
    check(watch.lost(22000).has_value(), "its own lost fixes do not count as fixes");
    watch.observe(fix, 22100);
    check(!watch.lost(25000) && watch.lost(25100), "a new fix restarts the wait");
}

void delivery() {
    std::string error;
    const auto bindings = parse_input_bindings(
        "touch example/Input onTouch (IFFIJ)V action x y pointer time\n"
        "location example/Input onFix (DDFIJ)V latitude longitude speed*3.6 course time\n", error);
    check(bindings.has_value(), "delivery bindings parse");
    Guest guest;

    guest.deliver(*bindings, TouchEvent{TouchPhase::move, 1, 400.5f, 240.0f, 0x100000002ULL});
    // env, this, action, x | y, pointer, time lo, time hi  (words 6-7 are already an even pair)
    check(guest.word(0) == 0xe0e0e0e0 && guest.word(1) == 0x0b0b0b0b, "env and receiver are in r0 and r1");
    check(guest.word(2) == 2 && guest.word(3) == std::bit_cast<std::uint32_t>(400.5f), "action code and float x are in r2 and r3");
    check(guest.word(4) == std::bit_cast<std::uint32_t>(240.0f) && guest.word(5) == 1, "y and pointer are the first stack words");
    check(guest.word(6) == 2 && guest.word(7) == 1, "a long is passed low word first");
    check(guest.word(8) == 1, "the handler ran once");

    guest.deliver(*bindings, LocationFix{37.5665, 126.978, 30, 10, 271.6, 5, 1700000000123ULL});
    // env, this, lat lo, lat hi | lon lo, lon hi, speed, course, (time on the next even pair, beyond the record)
    const auto lat = std::bit_cast<std::uint64_t>(37.5665), lon = std::bit_cast<std::uint64_t>(126.978);
    check(guest.word(2) == static_cast<std::uint32_t>(lat) && guest.word(3) == static_cast<std::uint32_t>(lat >> 32), "a double fills r2:r3");
    check(guest.word(4) == static_cast<std::uint32_t>(lon) && guest.word(5) == static_cast<std::uint32_t>(lon >> 32), "the next double is on the stack");
    check(guest.word(6) == std::bit_cast<std::uint32_t>(36.0f) && guest.word(7) == 272, "scaled float and rounded int follow");
    check(guest.word(8) == 2, "the handler ran twice");

    // Alignment: (I D) leaves r3 unused; (I J) likewise; an odd stack word is padded.
    NativeBinding padded{"example/Input", "m", "(IDIJ)V",
                         {{InputSource::constant, 5}, {InputSource::latitude, 1}, {InputSource::constant, 6}, {InputSource::time, 1}}};
    const auto words = native_call_words(padded, LocationFix{1.5, 0, 0, 0, 0, 0, 9}, {}, 1, 2, error);
    const auto bits = std::bit_cast<std::uint64_t>(1.5);
    check(words && *words == std::vector<std::uint32_t>{1, 2, 5, 0, static_cast<std::uint32_t>(bits), static_cast<std::uint32_t>(bits >> 32), 6, 0, 9, 0},
          "64-bit values start at even words");

    NativeBinding extremes{"example/Input", "m", "(IIJZ)V",
                           {{InputSource::constant, 1e12}, {InputSource::constant, -1e12}, {InputSource::constant, -2.5}, {InputSource::speed, 1}}};
    const auto clamped = native_call_words(extremes, LocationFix{0, 0, 0, -1, 0, 0, 0}, {}, 0, 0, error);
    check(clamped && (*clamped)[2] == 0x7fffffff && (*clamped)[3] == 0x80000000 && (*clamped)[4] == 0xfffffffe &&
          (*clamped)[5] == 0xffffffff && (*clamped)[6] == 0xffffffff, "integers saturate and negatives sign-extend");

    // A text argument is one word: the handle the caller returns for the sentence.
    NativeBinding text{"example/Input", "onSentence", "(Ljava/lang/String;)V", {{InputSource::gga, 1}}};
    std::string seen;
    const LocationFix fix{1, 2, 3, 4, 5, 6, 7};
    const auto with_text = native_call_words(text, fix, {}, 1, 2, error, [&](std::size_t index, const std::string& s) {
        seen = std::to_string(index) + s;
        return std::uint32_t{0xe0000004};
    });
    check(with_text && *with_text == std::vector<std::uint32_t>{1, 2, 0xe0000004} && seen == "0" + nmea_gga(fix), "String argument is the sentence's handle");
    check(!native_call_words(text, fix, {}, 1, 2, error) && error.find("String") != std::string::npos, "a text argument needs a String maker");
    check(native_call_count(text, fix) == 1 && native_call_count(text, TouchEvent{}) == 1, "one sentence, one call");

    // gsv is several sentences: one call per sentence, `part` picks it.
    NativeBinding sky{"example/Input", "onSentence", "(Ljava/lang/String;)V", {{InputSource::gsv, 1}}};
    const auto gsv = nmea_gsv(fix);
    check(native_call_count(sky, fix) == gsv.size(), "a GSV binding is called once per sentence");
    std::vector<std::string> parts;
    for (std::size_t part = 0; part < native_call_count(sky, fix); ++part)
        check(native_call_words(sky, fix, {}, 1, 2, error, [&](std::size_t, const std::string& s) { parts.push_back(s); return std::uint32_t{1}; }, part).has_value(),
              "GSV part words are built");
    check(parts == gsv, "each call carries the next GSV sentence");

    NativeBinding mismatched{"example/Input", "m", "(II)V", {{InputSource::x, 1}}};
    check(!native_call_words(mismatched, TouchEvent{}, {}, 0, 0, error) && !error.empty(), "argument count mismatch is an error");
    NativeBinding reference{"example/Input", "m", "([B)V", {{InputSource::x, 1}}};
    check(!native_call_words(reference, TouchEvent{}, {}, 0, 0, error), "reference parameter is an error");
}
}

int main() {
    try {
        queue();
        bindings_text();
        nmea();
        location_watch();
        delivery();
        std::cout << "input tests passed: " << checks << " checks\n";
    } catch (const std::exception& error) {
        std::cerr << "input test failed: " << error.what() << '\n';
        return 1;
    }
}
