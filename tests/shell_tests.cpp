// SPDX-License-Identifier: GPL-2.0-or-later
#include "shell/data_file.hpp"
#include "shell/render_config.hpp"
#include "shell/session.hpp"
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
    atlan::shell::GuestSession session({library, 100000}, collector.log());
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
}
int main() {
    try {
        scratch = fs::temp_directory_path() / ("atlan-shell-" + std::to_string(::getpid()));
        fs::create_directories(scratch);
        static_assert(atlan::shell::render_size(852, 393, 3).width == 2556);
        static_assert(atlan::shell::render_size(852, 393, 3, 0.5).height == 590);
        static_assert(atlan::shell::render_size(852, 393, 3, 0.01).width == 639);
        data_files();
        sessions();
        fs::remove_all(scratch);
        std::cout << "shell tests passed: " << checks << " checks\n";
    } catch (const std::exception& error) {
        std::cerr << "shell test failed: " << error.what() << '\n';
        return 1;
    }
}
