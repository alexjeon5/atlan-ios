// SPDX-License-Identifier: GPL-2.0-or-later
#include "adapters/atlan/charset_conversion.hpp"
#include "adapters/atlan/quad_geometry.hpp"
#include "adapters/atlan/speed_readout.hpp"
#include "atlan/audio.hpp"
#include "shell/coordinate.hpp"
#include "shell/data_file.hpp"
#include "shell/render_config.hpp"
#include "shell/guest_ini.hpp"
#include "shell/java_services.hpp"
#include "atlan/jni.hpp"
#include "shell/log_file.hpp"
#include "shell/log_filter.hpp"
#include "shell/session.hpp"
#include "shell/surface.hpp"
#include <algorithm>
#include <array>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <mutex>
#include <span>
#include <sys/stat.h>
#include <stdexcept>
#include <vector>
#include <unistd.h>

namespace {
unsigned checks{};
void check(bool result, const char* message) {
    ++checks;
    if (!result) throw std::runtime_error(message);
}
void put16(std::vector<std::byte>& data, std::size_t at, std::uint16_t value) {
    data.at(at) = std::byte(value & 255); data.at(at + 1) = std::byte(value >> 8);
}
void put32(std::vector<std::byte>& data, std::size_t at, std::uint32_t value) {
    for (unsigned i = 0; i < 4; ++i) data.at(at + i) = std::byte((value >> (8 * i)) & 255);
}
// This entirely synthetic ELF has no section headers. It exercises an
// unaligned writable PT_LOAD, BSS, SysV symbols, REL, PLT and constructors.
std::vector<std::byte> fixture() {
    std::vector<std::byte> data(0x1300);
    put32(data,0,0x464c457f); data[4]=std::byte{1};data[5]=std::byte{1};data[6]=std::byte{1};
    put16(data,16,3);put16(data,18,40);put32(data,20,1);put32(data,28,52);
    put32(data,36,0x05000000);put16(data,40,52);put16(data,42,32);put16(data,44,4);
    const auto ph = [&](unsigned i, std::uint32_t type, std::uint32_t offset,
                        std::uint32_t address, std::uint32_t filesz, std::uint32_t memsz,
                        std::uint32_t flags, std::uint32_t align) {
        const auto at = 52 + i * 32;
        put32(data,at,type);put32(data,at+4,offset);put32(data,at+8,address);
        put32(data,at+16,filesz);put32(data,at+20,memsz);put32(data,at+24,flags);put32(data,at+28,align);
    };
    ph(0,1,0,0,0x800,0x800,5,4096);
    ph(1,1,0x1100,0x2100,0x200,0x1000,6,4096);
    ph(2,2,0x1100,0x2100,0x98,0x98,6,4);
    ph(3,0x6474e552,0x1100,0x2100,0,0x100,4,1); // partial RELRO must not seal its final page
    put32(data,0x400,0xe0800001); // add r0,r0,r1
    put32(data,0x404,0xe12fff1e); // bx lr
    put16(data,0x408,0x3001);put16(data,0x40a,0x4770); // Thumb: adds r0,#1; bx lr
    const std::array<std::pair<std::uint32_t,std::uint32_t>,19> tags{{
        {5,0x200},{10,48},{6,0x280},{11,16},{4,0x300},
        {17,0x340},{18,24},{19,8},{23,0x358},{2,8},{20,17},
        {1,15},{25,0x2230},{27,8},{12,0x400},{14,1},{30,2},{0x6ffffffb,1},{0,0}}};
    for (std::size_t i=0;i<tags.size();++i) {
        put32(data,0x1100+i*8,tags[i].first);put32(data,0x1104+i*8,tags[i].second);
    }
    constexpr char names[]="\0add\0increment\0libexample.so\0external\0weak\0";
    for (std::size_t i=0;i<sizeof(names);++i) data[0x200+i]=std::byte(names[i]);
    const auto symbol = [&](unsigned i, unsigned name, unsigned value, unsigned info, unsigned section) {
        const auto at = 0x280+i*16;
        put32(data,at,name);put32(data,at+4,value);put32(data,at+8,0);
        data[at+12]=std::byte(info);put16(data,at+14,static_cast<std::uint16_t>(section));
    };
    symbol(1,1,0x400,0x12,1);symbol(2,5,0x409,0x12,1);
    symbol(3,29,0,0x12,0);symbol(4,38,0,0x22,0);
    put32(data,0x300,1);put32(data,0x304,5); // nbucket, nchain
    put32(data,0x340,0x2200);put32(data,0x344,23); // RELATIVE
    put32(data,0x348,0x2204);put32(data,0x34c,(2<<8)|2); // ABS32 Thumb + addend
    put32(data,0x350,0x2208);put32(data,0x354,(4<<8)|21); // weak GLOB_DAT
    put32(data,0x358,0x220c);put32(data,0x35c,(3<<8)|22); // imported JUMP_SLOT
    put32(data,0x1200,0x2240);put32(data,0x1204,4);put32(data,0x1208,0x1234);put32(data,0x120c,0x4444);
    // Init array slots themselves are relocated with RELATIVE in a separate test.
    put32(data,0x1230,0);put32(data,0x1234,UINT32_MAX);
    return data;
}

// A synthetic app for the adapter path: exports JNI_OnLoad and four entry points (init, surf, draw, quit) that only
// return a constant. quit returns 1, so an interactive session ends at its first exit poll.
std::vector<std::byte> app_fixture() {
    std::vector<std::byte> data(0x1200);
    put32(data,0,0x464c457f); data[4]=std::byte{1};data[5]=std::byte{1};data[6]=std::byte{1};
    put16(data,16,3);put16(data,18,40);put32(data,20,1);put32(data,28,52);
    put32(data,36,0x05000000);put16(data,40,52);put16(data,42,32);put16(data,44,3);
    const auto ph = [&](unsigned i, std::uint32_t type, std::uint32_t offset, std::uint32_t address,
                        std::uint32_t size, std::uint32_t flags, std::uint32_t align) {
        const auto at = 52 + i * 32;
        put32(data,at,type);put32(data,at+4,offset);put32(data,at+8,address);
        put32(data,at+16,size);put32(data,at+20,size);put32(data,at+24,flags);put32(data,at+28,align);
    };
    ph(0,1,0,0,0x800,5,4096);
    ph(1,1,0x1100,0x2100,0x100,6,4096);
    ph(2,2,0x1100,0x2100,0x30,6,4);
    const std::array<std::pair<std::uint32_t,std::uint32_t>,6> tags{{{5,0x200},{10,0x40},{6,0x280},{11,16},{4,0x300},{0,0}}};
    for (std::size_t i=0;i<tags.size();++i) { put32(data,0x1100+i*8,tags[i].first);put32(data,0x1104+i*8,tags[i].second); }
    constexpr char names[]="\0JNI_OnLoad\0init\0surf\0draw\0quit\0";
    for (std::size_t i=0;i<sizeof(names);++i) data[0x200+i]=std::byte(names[i]);
    const std::array<std::pair<unsigned, std::uint32_t>, 5> functions{{{1,6},{12,0},{17,0},{22,1},{27,1}}};  // name, r0
    for (std::size_t i=0;i<functions.size();++i) {
        const auto code = static_cast<std::uint32_t>(0x400 + i * 8);
        put32(data,code,0xe3a00000 | functions[i].second);  // mov r0,#value
        put32(data,code+4,0xe12fff1e);                     // bx lr
        const auto at = 0x280 + (i + 1) * 16;
        put32(data,at,functions[i].first);put32(data,at+4,code);put32(data,at+8,8);
        data[at+12]=std::byte{0x12};put16(data,at+14,1);
    }
    put32(data,0x300,1);put32(data,0x304,6); // nbucket, nchain
    return data;
}

namespace fs = std::filesystem;
fs::path scratch;
std::string write(const char* name, std::span<const std::byte> bytes) {
    const auto path = scratch / name;
    std::ofstream out(path, std::ios::binary);
    out.write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
    return path.string();
}
void data_files() {
    using atlan::shell::inspect_data_file;
    check(!inspect_data_file((scratch / "missing").string()).ok, "missing file is rejected");
    check(!inspect_data_file(scratch.string()).ok, "directory is rejected");
    check(!inspect_data_file(write("empty", {})).ok, "empty file is rejected");
    std::vector<std::byte> data(10000, std::byte{7});
    data[0] = std::byte{'M'}; data[1] = std::byte{'A'};
    const auto path = write("map", data);
    const auto ok = inspect_data_file(path, "MA");
    check(ok.ok && ok.size == 10000 && ok.message.empty(), "valid file accepted in place");
    check(!inspect_data_file(path, "XX").ok, "wrong header rejected");
    check(!inspect_data_file(path, {}, 20000).ok, "short file rejected");
    const auto tiny = write("tiny", std::span<const std::byte>(data).first(3));
    check(inspect_data_file(tiny, "MA").ok, "file smaller than one window is readable");
    ::chmod(path.c_str(), 0);
    if (::geteuid() != 0) check(!inspect_data_file(path).ok, "unreadable file rejected");
    ::chmod(path.c_str(), 0600);
    // Replacement is an overwrite; the next inspection sees the new content only.
    std::fill(data.begin(), data.end(), std::byte{9});
    write("map", data);
    check(!inspect_data_file(path, "MA").ok, "replaced file is validated afresh");
}
struct Collector {
    std::mutex lock;
    std::vector<std::string> lines;
    atlan::shell::GuestSession::Log log() {
        return [this](const std::string& line) { std::lock_guard guard(lock); lines.push_back(line); };
    }
};
atlan::shell::SessionState run(const std::string& library, Collector& collector) {
    atlan::shell::SessionConfig config;
    config.library_path = library;
    config.initializer_budget = 100000;
    atlan::shell::GuestSession session(config, collector.log());
    check(session.start(), "session thread starts");
    check(!session.start(), "session cannot start twice");
    session.join();
    return session.state();
}
void sessions() {
    using atlan::shell::SessionState;
    Collector missing;
    check(run((scratch / "nolib").string(), missing) == SessionState::failed, "missing library fails");
    check(!missing.lines.empty() && missing.lines.back().starts_with("load failed"), "failure is reported");
    std::vector<std::byte> junk(4096, std::byte{1});
    Collector garbage;
    check(run(write("junk.so", junk), garbage) == SessionState::failed, "non-ELF input fails");
    Collector good;
    const auto image = fixture();
    check(run(write("ok.so", image), good) == SessionState::finished, "synthetic library completes");
    check(good.lines.size() == 2 && good.lines.back().starts_with("initializers_completed="),
          "completion is logged");
}
// The session drives an app only through its adapter: entry points, Java phase and deliveries.
struct TestAdapter final : atlan::shell::AppAdapter {
    std::string library;
    unsigned installs{}, uninstalls{}, deliveries{}, unpacks{};
    bool asked_for_requests{};
    std::string find_library(const std::string&) const override { return library; }
    void unpack_library(std::vector<std::byte>&, const atlan::shell::SessionLog&) override { ++unpacks; }
    void install_java(atlan::shell::AppHost& host, bool frame_requests) override {
        ++installs;
        asked_for_requests = frame_requests;
        check(host.function("draw") != 0 && host.function("missing") == 0, "host finds exported functions");
        std::string error;
        check(host.audio().open({44100, 2}, error) != nullptr, "the host offers the session's audio device");
    }
    void uninstall_java() override { ++uninstalls; }
    atlan::shell::AppStartup startup() const override {
        atlan::shell::AppStartup startup;
        startup.receiver_class = "test/App";
        startup.init = "init";
        startup.surface_calls = {{"surf", true}};
        startup.render = "draw";
        startup.exit_poll = "quit";
        return startup;
    }
    bool deliver(atlan::shell::AppHost&) override { ++deliveries; return true; }
};
void adapter_sessions() {
    Collector collector;
    auto adapter = std::make_shared<TestAdapter>();
    adapter->library = write("app.so", app_fixture());
    atlan::shell::SessionConfig config;
    config.adapter = adapter;  // no library_path: the adapter finds it
    config.render_frames = 2;
    config.interactive = true;
    config.render_on_demand = true;
    config.frame_interval_ms = 5;
    bool exited = false;
    config.on_guest_exit = [&exited] { exited = true; };
    atlan::shell::GuestSession session(config, collector.log());
    check(session.start(), "adapter session starts");
    session.join();
    check(session.state() == atlan::shell::SessionState::finished, "adapter session finishes");
    const auto has = [&](const std::string& line) {
        return std::find(collector.lines.begin(), collector.lines.end(), line) != collector.lines.end();
    };
    check(has("JNI_OnLoad returned 0x6") && has("init returned 0x0") && has("surf returned 0x0"), "startup runs the adapter's entry points");
    check(std::count(collector.lines.begin(), collector.lines.end(), std::string("draw returned 0x1")) == 2, "first frames use the adapter's frame call");
    check(has("frames every interval"), "an adapter without frame requests draws every interval");
    check(has("guest exit") && exited, "the adapter's exit poll ends the session");
    check(adapter->unpacks == 1 && adapter->installs == 1 && adapter->uninstalls == 1 && adapter->asked_for_requests,
          "the Java phase is installed once and torn down");
    check(adapter->deliveries >= 3, "deliveries run before every frame");
}
}
// The app's home button reaches the host as message 5 of MainActivity.SendMsgFromAtlan, the end of its splash as
// message 13; other messages reach neither.
void home_button() {
    constexpr std::uint32_t data = 0x4000, stack = 0x80000, sentinel = 0xffff0000;
    atlan::Runtime runtime;
    atlan::ImportDispatcher dispatcher;
    runtime.memory.map(data, 0x1000, atlan::Permissions::read_write);
    runtime.memory.map(stack - 0x1000, 0x1000, atlan::Permissions::read_write);
    dispatcher.attach(atlan::elf::Image{});
    atlan::JavaBootstrap java(runtime, dispatcher, {});
    unsigned homes = 0;
    std::vector<std::uint32_t> splash_ends;
    atlan::shell::JavaServices::Config config;
    config.use_connection_manager = false;
    config.on_home = [&homes] { ++homes; };
    config.on_splash_end = [&splash_ends](std::uint32_t param) { splash_ends.push_back(param); };
    atlan::shell::JavaServices services(config);
    services.install(java, [](const std::string&, const std::string&, const std::string&) { return 0u; });
    const auto text = [&](std::uint32_t at, const std::string& value) {
        std::vector<std::byte> bytes(value.size() + 1);
        for (std::size_t i = 0; i < value.size(); ++i) bytes[i] = std::byte(static_cast<unsigned char>(value[i]));
        check(runtime.memory.write(at, bytes), "string written to guest memory");
    };
    text(data, "SendMsgFromAtlan");
    text(data + 0x40, "(III[B)V");
    std::array<std::byte, 4> word{};
    const auto read32 = [&](std::uint32_t at) {
        check(runtime.memory.read(at, word), "guest word read");
        return std::to_integer<std::uint32_t>(word[0]) | std::to_integer<std::uint32_t>(word[1]) << 8 |
               std::to_integer<std::uint32_t>(word[2]) << 16 | std::to_integer<std::uint32_t>(word[3]) << 24;
    };
    const auto table = read32(java.env());
    const auto invoke = [&](std::size_t slot, std::vector<std::uint32_t> args) {
        args.insert(args.begin(), java.env());
        const auto outcome = dispatcher.call(runtime, read32(table + 4 * static_cast<std::uint32_t>(slot)), args, stack, sentinel, 1000);
        check(outcome.stop == atlan::Stop::returned, "JNI call returns");
        return static_cast<std::uint32_t>(runtime.cpu.registers[0]);
    };
    const auto cls = java.class_handle("com/atlan/MainActivity");
    const auto activity = java.new_object(cls);
    const auto method = invoke(33, {cls, data, data + 0x40});  // GetMethodID
    (void)invoke(61, {activity, method, 17, 0, 0, 0});        // CallVoidMethod: navigation start
    check(homes == 0, "other messages do not press home");
    (void)invoke(61, {activity, method, 5, 1, 0, 0});
    check(homes == 1, "message 5 presses home");
    (void)invoke(61, {activity, method, 5, 1, 0, 0});
    check(homes == 2, "every press is delivered");
    check(splash_ends.empty(), "other messages do not end the splash");
    (void)invoke(61, {activity, method, 13, 0, 0, 0});
    (void)invoke(61, {activity, method, 13, 1, 0, 0});
    check(splash_ends == std::vector<std::uint32_t>{0, 1} && homes == 2, "message 13 ends the splash with its param");
}
// The app's text conversion as the host does it (the comparison with the guest's own code runs at install).
void text_conversion() {
    using namespace atlan::shell::charset;
    const auto bytes = [](std::vector<std::uint8_t> v) {
        return [v](std::uint32_t at) -> std::optional<std::uint8_t> { return at < v.size() ? std::optional<std::uint8_t>(v[at]) : std::nullopt; };
    };
    const auto units = [](std::vector<std::uint16_t> v) {
        return [v](std::uint32_t at) -> std::optional<std::uint16_t> { return at / 2 < v.size() ? std::optional<std::uint16_t>(v[at / 2]) : std::nullopt; };
    };
    std::vector<std::uint16_t> wide;
    const auto put_wide = [&](std::uint16_t u) { wide.push_back(u); return true; };
    std::vector<std::uint8_t> narrow;
    const auto put_narrow = [&](std::uint8_t b) { narrow.push_back(b); return true; };

    check(decode(0xB0, 0xA1) == u'\uAC00' && decode(0x81, 0x41) == u'\uAC02' && decode('A', 0) == u'A', "CP949 codes");
    check(decode(0xC9, 0xA1) == u'\uE000' && decode(0xFE, 0xFE) == u'\uE0BB', "user-defined rows map onto the Private Use Area");
    check(!decode(0x80, 0x41) && !decode(0xFF, 0xA1) && !decode(0xA2, 0xE8) && !decode(0xB0, 0x40), "unmapped codes");
    check(encode(u'\uAC00') == 0xB0A1 && encode(u'\uE05E') == 0xFEA1 && encode(u'z') == 'z', "encoding");
    check(!encode(u'\u0080') && !encode(char16_t{0xD800}) && !encode(u'\u327E') && !encode(u'\uFFFE'), "unmappable code points");

    auto left = to_wide(bytes({'a', 0xC7, 0xD1, 0}), 4, put_wide, 100);
    check(left == 94u && wide == std::vector<std::uint16_t>{'a', 0xD55C, 0}, "conversion with the terminator");
    wide.clear();
    left = to_wide(bytes({'a', 0x80, 'b'}), 3, put_wide, 100);
    check(left == 98u && wide == std::vector<std::uint16_t>{'a'}, "conversion stops at a byte it cannot convert");
    wide.clear();
    left = to_wide(bytes({'a', 0xC7}), 2, put_wide, 100);
    check(left == 98u && wide.size() == 1, "an incomplete character at the end is left");
    wide.clear();
    left = to_wide(bytes({'a', 'b', 'c'}), 3, put_wide, 5);
    check(left == 1u && wide.size() == 2, "conversion stops when the next unit does not fit");
    left = to_multibyte(units({'a', 0xD55C, 'b'}), 6, put_narrow, 3);
    check(left == 0u && narrow == std::vector<std::uint8_t>{'a', 0xC7, 0xD1}, "a character of two bytes");
    narrow.clear();
    left = to_multibyte(units({'a', 0xD55C}), 4, put_narrow, 2);
    check(left == 1u && narrow == std::vector<std::uint8_t>{'a'}, "two bytes that do not fit are not split");
    narrow.clear();
    left = to_multibyte(units({'a', 0xD800, 'b'}), 6, put_narrow, 10);
    check(left == 9u && narrow.size() == 1, "a surrogate stops conversion");
    narrow.clear();
    left = to_multibyte(units({'a', 'b'}), 3, put_narrow, 10);
    check(left == 9u && narrow.size() == 1, "an odd last byte is left");

    auto t = table_to_wide(bytes({'a', 0x80, 0xC7, 0xD1, 0xC9, 0xA1, 0}), 6, 100);
    check(t && t->units == std::vector<char16_t>{u'a', u'\u0080', u'\uD55C', u'?'} && t->terminator, "table conversion with '?' for unmapped codes");
    t = table_to_wide(bytes({'a', 'b', 'c', 0}), 3, 2);
    check(t && t->units.size() == 2 && !t->terminator, "no terminator without room");
    t = table_to_wide(bytes({'a', 0xC7, 0xD1, 'b', 'c', 0}), 2, 100);
    check(t && t->units.size() == 4 && t->bytes_read == 6, "an odd count runs on to the NUL");
    t = table_to_wide(bytes({'a', 0xFE, 0xA1, 0}), 3, 100);
    check(t && t->outside_table, "codes beyond the table are left to the guest");
    check(!table_to_wide(bytes({'a', 0xB0}), 3, 100), "unreadable input");
}
// Speeds above the app's one-byte 255 km/h: what a replaced load shows, and the instructions it accepts.
void speed_readout() {
    using namespace atlan::shell::speed;
    check(fix_kmh(100.0 / 3.6, 5) == 100 && fix_kmh(312.0 / 3.6, 5) == 312 && fix_kmh(-1, 5) == 0, "fix speed in km/h");
    check(!fix_kmh(30, -1), "a lost signal has no speed");
    check(shown_speed(255, 312) == 312 && shown_speed(255, 200) == 255 && shown_speed(255, -1) == 255, "only a saturated byte");
    check(shown_speed(120, 312) == 120 && shown_speed(0, 900) == 0, "an unsaturated byte is shown as it is");
    const auto fix = make_fix_speed();
    const auto sets = speed_readout_sets(fix);
    check(sets.size() == 2, "one set per screen");
    for (const auto& set : sets) {
        check(set.members.size() == 1 && set.instructions.size() == 1, "one load each");
        const auto& factory = set.instructions.begin()->second;
        check(factory(0xe5d12003) && factory(0xe5d40fff), "LDRB with an immediate offset");
        check(!factory(0xe5d1f003) && !factory(0xe5df2003), "not with PC");
        check(!factory(0x05d12003) && !factory(0xe5512003) && !factory(0xe7d12003) && !factory(0xe5f12003), "nothing else");
    }
}
// The app's geometry as the host computes it (the comparison with the guest's own code runs at install).
void quad_geometry() {
    using namespace atlan::shell::geometry;
    check(orientation({0, 0}, {1, 0}, {0, 1}) == 1 && orientation({0, 0}, {0, 1}, {1, 0}) == -1, "orientation sign");
    check(orientation({0, 0}, {2, 2}, {5, 5}) == 0, "collinear points");
    check(orientation({0, 0}, {65536, 0}, {0, 65536}) == 0, "the cross product wraps at 32 bits");
    check(segment_relation({0, 0}, {2, 2}, {0, 2}, {2, 0}) == 1, "crossing segments");
    check(segment_relation({0, 0}, {1, 0}, {0, 1}, {1, 1}) == -1, "parallel segments apart");
    check(segment_relation({0, 0}, {2, 0}, {2, 0}, {3, 1}) == 0, "segments touching at an end");
    check(segment_relation({2, 0}, {0, 0}, {3, 1}, {2, 0}) == 0, "ends given in either order");
    check(segment_relation({0, 0}, {1, 0}, {5, 0}, {6, 0}) == 0, "collinear segments compare one end only, as the app does");
    const std::array<Point, 4> square{Point{0, 0}, Point{10, 0}, Point{10, 10}, Point{0, 10}};
    check(inside_polygon(square, {5, 5}) == 1 && inside_polygon(square, {15, 5}) == 0 && inside_polygon(square, {5, -1}) == 0, "point in polygon");
    auto o = rectangle_overlap(square, {2, 2}, {4, 4});
    check(o.any && o.corners && o.low == Point{2, 2} && o.high == Point{4, 4}, "a rectangle inside the quad");
    o = rectangle_overlap(square, {5, 5}, {15, 15});
    check(o.any && !o.corners && o.low == Point{5, 5} && o.high == Point{10, 10} && o.points.size() == 4, "the overlap of a corner of the rectangle");
    // A fifth inside point shares its slot with the first edge intersection, which overwrites it (the app's
    // result for this input, found by the install-time comparison).
    const std::array<Point, 4> wide{Point{-202056, -441616}, Point{996010029, -621927}, Point{-1, -4832207}, Point{-1, 482558538}};
    o = rectangle_overlap(wide, {495996, 342190}, {284572, 1});
    check(o.any && o.low == Point{-202056, -4832207} && o.high == Point{996010029, 342190}, "an inside point overwritten by an intersection");
    o = rectangle_overlap(square, {20, 20}, {30, 30});
    check(!o.any && o.points.empty(), "no overlap");
}
void coordinates() {
    using atlan::shell::parse_coordinate;
    const auto near = [](std::optional<atlan::shell::GeoPoint> p, double lat, double lon) {
        return p && std::fabs(p->latitude - lat) < 1e-9 && std::fabs(p->longitude - lon) < 1e-9;
    };
    check(near(parse_coordinate("37.566535, 126.977969"), 37.566535, 126.977969), "comma-separated pair");
    check(near(parse_coordinate("  37.5 126 "), 37.5, 126), "space-separated pair");
    check(near(parse_coordinate("-33.8688,151.2093"), -33.8688, 151.2093), "signed latitude");
    check(near(parse_coordinate("37.56654\u00b0 N, 126.97797\u00b0 E"), 37.56654, 126.97797), "map app copy with hemispheres");
    check(near(parse_coordinate("33.8688 S 70.5 w"), -33.8688, -70.5), "south and west are negative");
    check(near(parse_coordinate("126.97797 E, 37.56654 N"), 37.56654, 126.97797), "longitude first when marked");
    check(near(parse_coordinate(".5, -.25"), 0.5, -0.25), "leading dots");
    for (const char* bad : {"", "37.5", "37.5, 126.9, 10", "91, 10", "10, 181", "abc 37 126", "37 N 126 N", "-37 S, 126",
                            "N 37, 126"})
        check(!parse_coordinate(bad), bad);
}

int main() {
    try {
        quad_geometry();
        speed_readout();
        text_conversion();
        home_button();
        scratch = fs::temp_directory_path() / ("atlan-shell-" + std::to_string(::getpid()));
        fs::create_directories(scratch);
        {
            atlan::shell::RepeatFilter filter(3);
            std::string line = "fopen x failed", perf = "perf frames: 1";
            check(filter.admit(line) && line == "fopen x failed", "first copy passes unchanged");
            check(filter.admit(line), "second copy passes");
            check(filter.admit(line) && line != "fopen x failed", "last copy is marked");
            line = "fopen x failed";
            check(!filter.admit(line) && !filter.admit(line), "further copies are hidden");
            for (int i = 0; i < 10; ++i) check(filter.admit(perf) && perf == "perf frames: 1", "perf lines always pass");
            std::string other = "fopen y failed";
            check(filter.admit(other), "a different line passes");
        }
        static_assert(atlan::shell::render_size(852, 393, 3).width == 2556);
        static_assert(atlan::shell::render_size(852, 393, 3, 0.5).height == 590);
        static_assert(atlan::shell::render_size(852, 393, 3, 0.01).width == 639);
        {
            atlan::shell::SurfaceState surface;
            check(surface.get().width == 0 && surface.generation() == 0, "surface starts unknown");
            surface.set({2556, 1179});
            check(surface.get().width == 2556 && surface.get().height == 1179 && surface.generation() == 1, "surface publishes size");
        }
        {
            using atlan::shell::ini_value, atlan::shell::ini_number, atlan::shell::ini_set;
            const std::string text = "A|1=5\r\nPATH|2= /x/y \r\nA2|1=7\r\nBAD\r\nA|1=6\r\n";
            check(ini_value(text, "PATH") == "/x/y" && ini_value(text, "A") == "5" && !ini_value(text, "BAD") && !ini_value(text, "ATH"),
                  "settings entries are found by exact key");
            check(ini_number(text, "A2") == 7u && !ini_number(text, "PATH") && !ini_number(text, "NONE"), "numeric settings parse");
            check(ini_set(text, "A", "12") == "A|1=12\r\nPATH|2= /x/y \r\nA2|1=7\r\nBAD\r\nA|1=12\r\n", "every entry of a key is replaced in place");
            check(ini_set(text, "NEW", "3") == text + "NEW|1=3\r\n" && ini_set("A|1=1", "B", "2") == "A|1=1\nB|1=2\n", "a missing key is appended");
            check(ini_set("A|1=1\r\n", "P", "/q", '2') == "A|1=1\r\nP|2=/q\r\n", "an appended entry takes the given type");
            check(ini_value("K|1=9", "K") == "9" && ini_set("K|1=9", "K", "10") == "K|1=10", "a last line without a newline works");
        }
        {
            const auto logs = scratch / "logs";
            fs::create_directories(logs);
            for (const char* name : {"session-20200101-000000.log", "session-20200102-000000.log", "session-20200103-000000.log", "other.txt"})
                std::ofstream(logs / name) << "old\n";
            std::string path;
            {
                atlan::shell::SessionLogFile log;
                log.write("before open is dropped");
                check(log.open(logs.string(), 3), "session log opens");
                log.write("first line");
                log.write("second line");
                path = log.path();
                std::ifstream in(path);  // readable while still open: every line is flushed
                std::string a, b;
                std::getline(in, a); std::getline(in, b);
                check(a.ends_with(" first line") && b.ends_with(" second line"), "lines reach the file as they are written");
            }
            check(!fs::exists(logs / "session-20200101-000000.log") && fs::exists(logs / "session-20200102-000000.log") &&
                  fs::exists(logs / "session-20200103-000000.log") && fs::exists(logs / "other.txt") && fs::exists(path),
                  "only the newest session logs are kept; other files are left alone");
        }
        coordinates();
        data_files();
        sessions();
        adapter_sessions();
        fs::remove_all(scratch);
        std::cout << "shell tests passed: " << checks << " checks\n";
    } catch (const std::exception& error) {
        std::cerr << "shell test failed: " << error.what() << '\n';
        return 1;
    }
}
