// SPDX-License-Identifier: GPL-2.0-or-later
#include "adapters/atlan/quad_geometry.hpp"
#include "shell/data_file.hpp"
#include "shell/render_config.hpp"
#include "shell/guest_ini.hpp"
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
int main() {
    try {
        quad_geometry();
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
