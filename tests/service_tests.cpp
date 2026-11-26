// SPDX-License-Identifier: GPL-2.0-or-later
#include "atlan/abi.hpp"
#include "atlan/jni.hpp"
#include "atlan/data_symbols.hpp"
#include "atlan/libc_extra.hpp"
#include "atlan/libc_services.hpp"
#include "atlan/mmap.hpp"
#include "atlan/stdio.hpp"
#include "atlan/string_services.hpp"
#include <array>
#include <cstring>
#include <fstream>
#include <iostream>
#include <stdexcept>

namespace {
unsigned checks{};
void check(bool result, const char* message) {
    ++checks;
    if (!result) throw std::runtime_error(message);
}
void put_text(atlan::Memory& memory, std::uint32_t at, const char* text) {
    check(memory.write(at, std::span<const std::byte>(reinterpret_cast<const std::byte*>(text), std::strlen(text) + 1)), "write text");
}
std::string text_at(atlan::Memory& memory, std::uint32_t at) {
    std::string out;
    check(atlan::read_guest_string(memory, at, 4096, out), "read text");
    return out;
}
std::uint32_t get32(atlan::Memory& memory, std::uint32_t at) {
    std::array<std::byte, 4> bytes{};
    check(memory.read(at, bytes), "read word");
    std::uint32_t v{};
    for (unsigned i = 0; i < 4; ++i) v |= std::to_integer<std::uint32_t>(bytes[i]) << (8 * i);
    return v;
}
void put32(atlan::Memory& memory, std::uint32_t at, std::uint32_t value) {
    std::array<std::byte, 4> bytes{};
    for (unsigned i = 0; i < 4; ++i) bytes[i] = std::byte((value >> (8 * i)) & 255);
    check(memory.write(at, bytes), "write word");
}
constexpr std::uint32_t data = 0x4000, stack = 0x80000, sentinel = 0xffff0000;
struct Trap { const char* name; std::uint32_t address; };
const Trap traps[] = {{"strcmp", 0xf0000000}, {"strncmp", 0xf0000004}, {"memcmp", 0xf0000008}, {"strncpy", 0xf000000c},
                      {"strchr", 0xf0000010}, {"strrchr", 0xf0000014}, {"snprintf", 0xf0000018}, {"vsnprintf", 0xf000001c},
                      {"sprintf", 0xf0000020}, {"mmap", 0xf0000024}, {"munmap", 0xf0000028}, {"fopen", 0xf000002c},
                      {"fclose", 0xf0000030}, {"fread", 0xf0000034}, {"fwrite", 0xf0000038}, {"fseek", 0xf000003c},
                      {"ftell", 0xf0000040}, {"stat", 0xf0000044}, {"__system_property_get", 0xf0000048},
                      {"feof", 0xf000004c}, {"opendir", 0xf0000050}, {"readdir", 0xf0000054}, {"closedir", 0xf0000058},
                      {"mkdir", 0xf000005c}, {"remove", 0xf0000060}, {"rename", 0xf0000064}, {"open", 0xf0000068},
                      {"read", 0xf000006c}, {"write", 0xf0000070}, {"close", 0xf0000074}, {"lseek", 0xf0000078},
                      {"fgets", 0xf000007c}, {"srand48", 0xf0000080}, {"lrand48", 0xf0000084}, {"umask", 0xf0000088},
                      {"fsync", 0xf000008c}, {"ftruncate", 0xf0000090}, {"strtok_r", 0xf0000094}, {"atoi", 0xf0000098}, {"strcasecmp", 0xf000009c}, {"strncasecmp", 0xf00000a0}, {"strstr", 0xf00000a4}, {"strtol", 0xf00000a8}, {"sqrt", 0xf00000ac}, {"strtok", 0xf00000b0}, {"setjmp", 0xf00000b4}, {"longjmp", 0xf00000b8}, {"qsort", 0xf00000bc}, {"cmp", 0xf00000c0},
                      {"memset", 0xf00000c4}, {"memcpy", 0xf00000c8}, {"memmove", 0xf00000cc}, {"strlen", 0xf00000d0}};
std::uint32_t trap(const char* name) {
    for (const auto& t : traps) if (std::strcmp(t.name, name) == 0) return t.address;
    throw std::logic_error("unknown trap");
}
struct Fixture {
    atlan::Runtime runtime;
    atlan::ImportDispatcher dispatcher;
    atlan::GuestMmap arena;
    std::filesystem::path root = std::filesystem::temp_directory_path() / "atlan-service-tests";
    std::map<std::string, std::string> properties{{"ro.test", "value"}};
    std::vector<std::string> opened;
    atlan::GuestStdio stdio{root, data + 0xf00, [this](const std::string& p, const std::string&, bool ok) { opened.push_back(p + (ok ? "+" : "-")); }};
    Fixture() {
        std::filesystem::remove_all(root);
        std::filesystem::create_directories(root / "dir");
        std::ofstream(root / "dir" / "a.txt", std::ios::binary) << "hello world";
        runtime.memory.map(data, 0x4000, atlan::Permissions::read_write);
        runtime.memory.map(stack - 0x1000, 0x1000, atlan::Permissions::read_write);
        atlan::elf::Image image;
        for (const auto& t : traps) image.trapped_imports.push_back({t.name, t.address, 2});
        dispatcher.attach(image);
        atlan::install_string_services(dispatcher);
        atlan::install_printf_services(dispatcher);
        atlan::install_system_properties(dispatcher, properties);
        atlan::install_libc_services(dispatcher);
        atlan::install_libc_extra(dispatcher);
        arena.install(dispatcher);
        stdio.install(dispatcher);
    }
    atlan::CallOutcome call(const char* name, std::initializer_list<std::uint32_t> args) {
        const std::vector<std::uint32_t> values(args);
        return dispatcher.call(runtime, trap(name), values, stack, sentinel, 10000);
    }
    std::uint32_t r0() { return static_cast<std::uint32_t>(runtime.cpu.registers[0]); }
};
void strings() {
    Fixture f;
    put_text(f.runtime.memory, data, "abc");
    put_text(f.runtime.memory, data + 0x10, "abd");
    check(f.call("strcmp", {data, data + 0x10}).stop == atlan::Stop::returned && static_cast<std::int32_t>(f.r0()) < 0, "strcmp orders");
    check(f.call("strncmp", {data, data + 0x10, 2}).stop == atlan::Stop::returned && f.r0() == 0, "strncmp equal prefix");
    check(f.call("memcmp", {data, data + 0x10, 3}).stop == atlan::Stop::returned && f.r0() != 0, "memcmp differs");
    check(f.call("strcmp", {data, 0x9000000}).stop == atlan::Stop::service_error, "strcmp faults on unmapped");
    check(f.call("strncpy", {data + 0x20, data, 6}).stop == atlan::Stop::returned && f.r0() == data + 0x20, "strncpy returns dst");
    check(text_at(f.runtime.memory, data + 0x20) == "abc" && get32(f.runtime.memory, data + 0x24) == 0, "strncpy pads with NUL");
    check(f.call("strchr", {data, 'c'}).stop == atlan::Stop::returned && f.r0() == data + 2, "strchr");
    check(f.call("strchr", {data, 'z'}).stop == atlan::Stop::returned && f.r0() == 0, "strchr miss");
    check(f.call("strchr", {data, 0}).stop == atlan::Stop::returned && f.r0() == data + 3, "strchr finds the terminator");
    put_text(f.runtime.memory, data + 0x30, "a/b/c");
    check(f.call("strrchr", {data + 0x30, '/'}).stop == atlan::Stop::returned && f.r0() == data + 0x30 + 3, "strrchr finds last");
}
void printf_family() {
    Fixture f;
    put_text(f.runtime.memory, data, "%s-%d");
    put_text(f.runtime.memory, data + 0x10, "xy");
    check(f.call("snprintf", {data + 0x100, 6, data, data + 0x10, 12345}).stop == atlan::Stop::returned && f.r0() == 8, "snprintf returns full length");
    check(text_at(f.runtime.memory, data + 0x100) == "xy-12", "snprintf truncates with NUL");
    check(f.call("snprintf", {data + 0x100, 0, data, data + 0x10, 1}).stop == atlan::Stop::returned && text_at(f.runtime.memory, data + 0x100) == "xy-12", "size 0 writes nothing");
    check(f.call("sprintf", {data + 0x200, data, data + 0x10, 7}).stop == atlan::Stop::returned && text_at(f.runtime.memory, data + 0x200) == "xy-7", "sprintf");
    put32(f.runtime.memory, data + 0x300, data + 0x10);
    put32(f.runtime.memory, data + 0x304, 99);
    put_text(f.runtime.memory, data + 0x40, "%s=%d");
    check(f.call("vsnprintf", {data + 0x400, 64, data + 0x40, data + 0x300}).stop == atlan::Stop::returned &&
          text_at(f.runtime.memory, data + 0x400) == "xy=99", "vsnprintf reads the va_list");
}
void mapping() {
    Fixture f;
    check(f.call("mmap", {0, 5000, 3, 0x22, 0xffffffff, 0}).stop == atlan::Stop::returned && f.r0() >= 0x90000000, "anonymous mmap");
    const auto a = f.r0();
    check(f.runtime.memory.write(a + 4999, std::array<std::byte, 1>{std::byte{1}}), "mapping is writable and page rounded");
    check(f.call("mmap", {0, 4096, 3, 0x22, 0xffffffff, 0}).stop == atlan::Stop::returned && f.r0() == a + 8192, "next mapping follows");
    check(f.call("mmap", {0, 4096, 3, 0x2, 3, 0}).stop == atlan::Stop::service_error, "file-backed mmap stops");
    check(f.call("mmap", {0, 4096, 7, 0x22, 0xffffffff, 0}).stop == atlan::Stop::service_error, "executable mmap stops");
    check(f.call("munmap", {a, 8192}).stop == atlan::Stop::returned && f.r0() == 0, "munmap");
    check(!f.runtime.memory.write(a, std::array<std::byte, 1>{}), "unmapped pages fault");
    check(f.call("munmap", {a, 4096}).stop == atlan::Stop::service_error, "double munmap is rejected");
    check(f.call("mmap", {0, 0x7fffffff, 3, 0x22, 0xffffffff, 0}).stop == atlan::Stop::returned && f.r0() == 0xffffffff, "oversize mmap fails with MAP_FAILED");
    check(f.call("mmap", {0, 4096, 3, 0x22, 0xffffffff, 0}).stop == atlan::Stop::returned && f.r0() == a, "freed space is reused");
}
void files() {
    Fixture f;
    put_text(f.runtime.memory, data, "/dir/a.txt");
    put_text(f.runtime.memory, data + 0x20, "r");
    check(f.call("fopen", {data, data + 0x20}).stop == atlan::Stop::returned && f.r0() != 0, "fopen existing file");
    const auto file = f.r0();
    check(f.call("fread", {data + 0x100, 1, 5, file}).stop == atlan::Stop::returned && f.r0() == 5 && text_at(f.runtime.memory, data + 0x100).rfind("hello", 0) == 0, "fread");
    check(f.call("ftell", {file}).stop == atlan::Stop::returned && f.r0() == 5, "ftell");
    check(f.call("fseek", {file, 0, 2}).stop == atlan::Stop::returned && f.r0() == 0, "fseek end");
    check(f.call("ftell", {file}).stop == atlan::Stop::returned && f.r0() == 11, "size via fseek/ftell");
    check(f.call("fread", {data + 0x100, 1, 4, file}).stop == atlan::Stop::returned && f.r0() == 0, "fread at end returns 0");
    check(f.call("feof", {file}).stop == atlan::Stop::returned && f.r0() == 1, "feof");
    check(f.call("fclose", {file}).stop == atlan::Stop::returned && f.r0() == 0 && f.stdio.open_files() == 0, "fclose");
    check(f.call("fread", {data + 0x100, 1, 1, file}).stop == atlan::Stop::service_error, "closed FILE is rejected");
    put_text(f.runtime.memory, data + 0x40, "/dir/missing");
    check(f.call("fopen", {data + 0x40, data + 0x20}).stop == atlan::Stop::returned && f.r0() == 0 && get32(f.runtime.memory, data + 0xf00) == 2, "missing file: NULL and ENOENT");
    put_text(f.runtime.memory, data + 0x60, "/../escape");
    check(f.call("fopen", {data + 0x60, data + 0x20}).stop == atlan::Stop::returned && f.r0() == 0 && get32(f.runtime.memory, data + 0xf00) == 13, "parent escape is refused");
    put_text(f.runtime.memory, data + 0x80, "/dir/new.bin");
    put_text(f.runtime.memory, data + 0xa0, "wb");
    check(f.call("fopen", {data + 0x80, data + 0xa0}).stop == atlan::Stop::returned && f.r0() != 0, "fopen for writing");
    const auto out = f.r0();
    check(f.call("fwrite", {data, 1, 4, out}).stop == atlan::Stop::returned && f.r0() == 4 && f.call("fclose", {out}).stop == atlan::Stop::returned, "fwrite and close");
    check(std::filesystem::file_size(f.root / "dir" / "new.bin") == 4, "write landed under the root");
    put_text(f.runtime.memory, data + 0xc0, "q");
    check(f.call("fopen", {data, data + 0xc0}).stop == atlan::Stop::service_error, "unsupported mode stops");
    check(f.call("stat", {data, data + 0x200}).stop == atlan::Stop::returned && f.r0() == 0 && get32(f.runtime.memory, data + 0x210) == 0100644 &&
          get32(f.runtime.memory, data + 0x230) == 11, "stat reports a regular file and its size");
    put_text(f.runtime.memory, data + 0xe0, "/dir");
    check(f.call("stat", {data + 0xe0, data + 0x200}).stop == atlan::Stop::returned && get32(f.runtime.memory, data + 0x210) == 040755, "stat reports a directory");
    check(f.call("stat", {data + 0x40, data + 0x200}).stop == atlan::Stop::returned && f.r0() == 0xffffffff, "stat of a missing path fails");
    check(f.opened.size() >= 3 && f.opened[0] == "/dir/a.txt+", "open attempts are traced");
}
void path_mapping() {
    Fixture f;
    std::filesystem::create_directories(f.root / "DATA" / "Sub");
    std::ofstream(f.root / "DATA" / "Sub" / "b.txt", std::ios::binary) << "W=1\nH=2\n";
    f.stdio.add_alias("/mnt/card/Data", "DATA");
    f.stdio.add_anchor("data", "DATA");
    f.stdio.add_read_filter("DATA/sub/B.TXT", [](std::string text) { return text + "X=3\n"; });
    put_text(f.runtime.memory, data + 0x80, "r");
    const auto reads = [&](const char* path, const char* expected) {
        put_text(f.runtime.memory, data, path);
        if (f.call("fopen", {data, data + 0x80}).stop != atlan::Stop::returned || f.r0() == 0) return false;
        const auto file = f.r0();
        const bool ok = f.call("fread", {data + 0x100, 1, 64, file}).stop == atlan::Stop::returned && f.r0() == std::strlen(expected) &&
                        std::memcmp(text_at(f.runtime.memory, data + 0x100).data(), expected, std::strlen(expected)) == 0;
        return f.call("fclose", {file}).stop == atlan::Stop::returned && ok;
    };
    check(reads("/mnt/card/Data/Sub/b.txt", "W=1\nH=2\nX=3\n"), "an aliased path reaches the folder and the read filter applies");
    check(reads("/storage/emulated/0/DATA/Sub/b.txt", "W=1\nH=2\nX=3\n"), "an anchor maps any absolute path through the named folder");
    check(reads("/x/data/sub/B.TXT", "W=1\nH=2\nX=3\n"), "anchors and filters ignore case");
    check(reads("/dir/a.txt", "hello world"), "paths without an anchor component are unchanged");
    put_text(f.runtime.memory, data, "/storage/other/Sub/b.txt");
    check(f.call("fopen", {data, data + 0x80}).stop == atlan::Stop::returned && f.r0() == 0, "an unanchored path is not redirected");
    put_text(f.runtime.memory, data, "/x/data/Sub/b.txt");
    put_text(f.runtime.memory, data + 0x80, "r+");
    check(f.call("fopen", {data, data + 0x80}).stop == atlan::Stop::returned && f.r0() != 0, "a writable open bypasses the filter");
    const auto file = f.r0();
    check(f.call("fread", {data + 0x100, 1, 64, file}).stop == atlan::Stop::returned && f.r0() == 8, "and sees the host file itself");
    check(f.call("fclose", {file}).stop == atlan::Stop::returned, "fclose");
    check(std::filesystem::equivalent(f.stdio.host_path("/x/data/sub"), f.root / "DATA" / "Sub") && f.stdio.host_path("/x/data/../..").empty(), "host_path resolves like the guest calls");
    put_text(f.runtime.memory, data + 0x80, "r");
    f.stdio.record_paths();
    check(reads("/mnt/card/Data/Sub/b.txt", "W=1\nH=2\nX=3\n"), "a read while paths are recorded");
    put_text(f.runtime.memory, data, "/mnt/card/Data/missing.txt");
    check(f.call("fopen", {data, data + 0x80}).stop == atlan::Stop::returned && f.r0() == 0, "a missing file");
    {
        const auto recorded = f.stdio.take_recorded_paths();
        check(recorded.size() == 2 && std::filesystem::equivalent(recorded[0], f.root / "DATA" / "Sub" / "b.txt") &&
              recorded[1] == f.root / "DATA" / "missing.txt", "the host paths the guest resolved are recorded, missing ones too");
        check(reads("/dir/a.txt", "hello world") && f.stdio.take_recorded_paths().empty(), "nothing is recorded after take");
    }
    {   // a mount serves a prefix from a folder outside the root; the more specific mount added first stays separate
        const auto outside = f.root.parent_path() / (f.root.filename().string() + "-mount");
        std::filesystem::remove_all(outside);
        std::filesystem::create_directories(outside / "Sub");
        std::filesystem::create_directories(f.root / "state");
        std::ofstream(outside / "Sub" / "Tile.bin", std::ios::binary) << "tile";
        f.stdio.add_alias("/storage/x", "MNT");
        f.stdio.add_mount("MNT/NAND", f.root / "state");
        f.stdio.add_mount("MNT", outside);
        put_text(f.runtime.memory, data + 0x80, "r");
        check(reads("/storage/x/sub/tile.bin", "tile"), "a mounted folder is read in place, ignoring case");
        put_text(f.runtime.memory, data, "/storage/x/NAND/new.txt");
        put_text(f.runtime.memory, data + 0x80, "wb");
        check(f.call("fopen", {data, data + 0x80}).stop == atlan::Stop::returned && f.r0() != 0 && f.call("fclose", {f.r0()}).stop == atlan::Stop::returned, "write through the inner mount");
        check(std::filesystem::exists(f.root / "state" / "new.txt") && !std::filesystem::exists(outside / "NAND"), "the inner mount keeps writes beneath the root");
        check(std::filesystem::equivalent(f.stdio.host_path("/storage/x"), outside) && f.stdio.host_path("/storage/x/../..").empty(), "host_path follows mounts and still refuses escapes");
        std::filesystem::remove_all(outside);
    }
}
void directories_and_descriptors() {
    Fixture f;
    put_text(f.runtime.memory, data, "/dir");
    check(f.call("opendir", {data}).stop == atlan::Stop::returned && f.r0() != 0, "opendir");
    const auto dir = f.r0();
    std::vector<std::string> names;
    for (;;) {
        check(f.call("readdir", {dir}).stop == atlan::Stop::returned, "readdir returns");
        if (f.r0() == 0) break;
        names.push_back(text_at(f.runtime.memory, f.r0() + 19));
    }
    check(names.size() == 3 && names[0] == "." && names[1] == ".." && names[2] == "a.txt", "listing is ., .. then sorted entries");
    check(f.call("closedir", {dir}).stop == atlan::Stop::returned && f.r0() == 0, "closedir");
    check(f.call("readdir", {dir}).stop == atlan::Stop::service_error, "a closed DIR is rejected");
    put_text(f.runtime.memory, data + 0x20, "/nope");
    check(f.call("opendir", {data + 0x20}).stop == atlan::Stop::returned && f.r0() == 0 && get32(f.runtime.memory, data + 0xf00) == 2, "missing directory: NULL and ENOENT");
    check(f.call("mkdir", {data + 0x20, 0777}).stop == atlan::Stop::returned && f.r0() == 0 && std::filesystem::is_directory(f.root / "nope"), "mkdir creates one level");
    check(f.call("mkdir", {data + 0x20, 0777}).stop == atlan::Stop::returned && f.r0() == 0xffffffff && get32(f.runtime.memory, data + 0xf00) == 17, "mkdir of an existing path is EEXIST");
    put_text(f.runtime.memory, data + 0x30, "");
    check(f.call("mkdir", {data + 0x30, 0777}).stop == atlan::Stop::returned && f.r0() == 0xffffffff && get32(f.runtime.memory, data + 0xf00) == 2, "mkdir of an empty path is ENOENT");
    put_text(f.runtime.memory, data + 0x40, "/a/b/c");
    check(f.call("mkdir", {data + 0x40, 0777}).stop == atlan::Stop::returned && f.r0() == 0xffffffff && get32(f.runtime.memory, data + 0xf00) == 2, "mkdir without a parent is ENOENT");
    // A trailing slash still names the directory (the app builds folder paths ending in '/').
    put_text(f.runtime.memory, data + 0x40, "/slash/");
    check(f.call("mkdir", {data + 0x40, 0777}).stop == atlan::Stop::returned && f.r0() == 0 && std::filesystem::is_directory(f.root / "slash"), "mkdir with a trailing slash creates the directory");
    put_text(f.runtime.memory, data + 0x40, "/slash/sub");
    check(f.call("mkdir", {data + 0x40, 0777}).stop == atlan::Stop::returned && f.r0() == 0 && std::filesystem::is_directory(f.root / "slash" / "sub"), "mkdir beneath it");
    put_text(f.runtime.memory, data + 0x40, "/slash/sub/");
    check(f.call("mkdir", {data + 0x40, 0777}).stop == atlan::Stop::returned && f.r0() == 0xffffffff && get32(f.runtime.memory, data + 0xf00) == 17, "mkdir of an existing directory with a trailing slash is EEXIST");
    check(f.call("opendir", {data + 0x40}).stop == atlan::Stop::returned && f.r0() != 0, "opendir with a trailing slash");
    check(f.call("closedir", {f.r0()}).stop == atlan::Stop::returned && f.r0() == 0, "closedir after a trailing slash");
    put_text(f.runtime.memory, data + 0x40, "/slash/sub/.");
    check(f.call("remove", {data + 0x40}).stop == atlan::Stop::returned && f.r0() == 0xffffffff && get32(f.runtime.memory, data + 0xf00) == 22 && std::filesystem::is_directory(f.root / "slash" / "sub"), "remove of dir/. is EINVAL and keeps the directory");
    put_text(f.runtime.memory, data + 0x40, "/slash/file/");
    check(f.call("open", {data + 0x40, 0102, 0644}).stop == atlan::Stop::returned && f.r0() == 0xffffffff && !std::filesystem::exists(f.root / "slash" / "file"), "open of a file path with a trailing slash fails");
    // POSIX descriptors: create, write, seek, read back, truncate, close, remove, rename.
    put_text(f.runtime.memory, data + 0x60, "/dir/fd.bin");
    check(f.call("open", {data + 0x60, 0102, 0644}).stop == atlan::Stop::returned && f.r0() >= 3, "open O_RDWR|O_CREAT");
    const auto fd = f.r0();
    put_text(f.runtime.memory, data + 0x80, "abcdef");
    check(f.call("write", {fd, data + 0x80, 6}).stop == atlan::Stop::returned && f.r0() == 6, "write");
    check(f.call("lseek", {fd, 2, 0}).stop == atlan::Stop::returned && f.r0() == 2, "lseek SEEK_SET");
    check(f.call("read", {fd, data + 0x100, 10}).stop == atlan::Stop::returned && f.r0() == 4 && text_at(f.runtime.memory, data + 0x100).rfind("cdef", 0) == 0, "read from the offset");
    check(f.call("lseek", {fd, 0, 2}).stop == atlan::Stop::returned && f.r0() == 6, "lseek SEEK_END");
    check(f.call("ftruncate", {fd, 3}).stop == atlan::Stop::returned && f.r0() == 0 && std::filesystem::file_size(f.root / "dir" / "fd.bin") == 3, "ftruncate");
    check(f.call("fsync", {fd}).stop == atlan::Stop::returned && f.r0() == 0, "fsync");
    check(f.stdio.descriptor_size(fd) == 3u, "descriptor size is visible to fstat");
    check(f.call("close", {fd}).stop == atlan::Stop::returned && f.r0() == 0 && f.stdio.open_descriptors() == 0, "close");
    check(f.call("read", {fd, data + 0x100, 1}).stop == atlan::Stop::returned && f.r0() == 0xffffffff && get32(f.runtime.memory, data + 0xf00) == 9, "closed descriptor is EBADF");
    put_text(f.runtime.memory, data + 0xa0, "/dir/fd2.bin");
    check(f.call("rename", {data + 0x60, data + 0xa0}).stop == atlan::Stop::returned && f.r0() == 0 && std::filesystem::exists(f.root / "dir" / "fd2.bin"), "rename");
    check(f.call("remove", {data + 0xa0}).stop == atlan::Stop::returned && f.r0() == 0 && !std::filesystem::exists(f.root / "dir" / "fd2.bin"), "remove");
    check(f.call("remove", {data + 0xa0}).stop == atlan::Stop::returned && f.r0() == 0xffffffff, "remove of a missing file fails");
    put_text(f.runtime.memory, data + 0xc0, "/../x");
    check(f.call("open", {data + 0xc0, 0101, 0644}).stop == atlan::Stop::returned && f.r0() == 0xffffffff && get32(f.runtime.memory, data + 0xf00) == 13, "open cannot escape the root");
    check(f.call("open", {data + 0x60, 0x1000000, 0}).stop == atlan::Stop::service_error, "unsupported open flags stop");
    check(f.call("umask", {077}).stop == atlan::Stop::returned, "umask call");
    check(f.r0() == 022, "umask returns the default mask first");
    check(f.call("umask", {022}).stop == atlan::Stop::returned, "umask call again");
    check(f.r0() == 077, "umask returns the previous mask");
    // fgets
    put_text(f.runtime.memory, data + 0xe0, "/dir/a.txt");
    put_text(f.runtime.memory, data + 0xf0, "r");
    check(f.call("fopen", {data + 0xe0, data + 0xf0}).stop == atlan::Stop::returned && f.r0() != 0, "fopen for fgets");
    const auto file = f.r0();
    check(f.call("fgets", {data + 0x100, 6, file}).stop == atlan::Stop::returned && f.r0() == data + 0x100 && text_at(f.runtime.memory, data + 0x100) == "hello", "fgets stops at n-1");
    check(f.call("fgets", {data + 0x100, 64, file}).stop == atlan::Stop::returned && text_at(f.runtime.memory, data + 0x100) == " world", "fgets reads to EOF");
    check(f.call("fgets", {data + 0x100, 64, file}).stop == atlan::Stop::returned && f.r0() == 0, "fgets at EOF is NULL");
    // lrand48 follows the POSIX LCG (srand48(1) -> 89400484, 976015093)
    check(f.call("srand48", {1}).stop == atlan::Stop::returned, "srand48");
    check(f.call("lrand48", {}).stop == atlan::Stop::returned && f.r0() == 89400484, "lrand48 #1");
    check(f.call("lrand48", {}).stop == atlan::Stop::returned && f.r0() == 976015093, "lrand48 #2");
    // _toupper_tab_ is a pointer variable to short[257]
    atlan::Memory memory;
    atlan::BionicData::map(memory);
    check(atlan::BionicData::symbol("_toupper_tab_") == atlan::BionicData::base && !atlan::BionicData::symbol("_tolower_tab_"), "only _toupper_tab_ is provided");
    check(get32(memory, atlan::BionicData::base) == atlan::BionicData::base + 0x100, "the variable holds the table pointer");
    std::array<std::byte, 2> entry{};
    check(memory.read(atlan::BionicData::base + 0x100 + 2 * ('a' + 1), entry) && entry[0] == std::byte{'A'}, "table[1+'a'] is 'A'");
    check(memory.read(atlan::BionicData::base + 0x100, entry) && entry[0] == std::byte{0xff} && entry[1] == std::byte{0xff}, "table[0] is EOF");
}
void properties_and_strings() {
    Fixture f;
    put_text(f.runtime.memory, data, "ro.test");
    check(f.call("__system_property_get", {data, data + 0x100}).stop == atlan::Stop::returned && f.r0() == 5 && text_at(f.runtime.memory, data + 0x100) == "value", "known property");
    put_text(f.runtime.memory, data + 0x20, "ro.unset");
    put_text(f.runtime.memory, data + 0x100, "junk");
    check(f.call("__system_property_get", {data + 0x20, data + 0x100}).stop == atlan::Stop::returned && f.r0() == 0 && text_at(f.runtime.memory, data + 0x100).empty(), "unset property is empty");
}
void tokenizing() {
    Fixture f;
    put_text(f.runtime.memory, data, ",a,,bc;d,");
    put_text(f.runtime.memory, data + 0x40, ",;");
    const auto save = data + 0x80;
    auto next = [&](std::uint32_t s) { return f.call("strtok_r", {s, data + 0x40, save}).stop == atlan::Stop::returned ? f.r0() : 0xdead; };
    const auto a = next(data);
    check(a == data + 1 && text_at(f.runtime.memory, a) == "a", "first token skips the leading delimiter");
    const auto b = next(0);
    check(b == data + 4 && text_at(f.runtime.memory, b) == "bc", "empty fields are skipped");
    const auto c = next(0);
    check(c == data + 7 && text_at(f.runtime.memory, c) == "d", "second delimiter set member");
    check(next(0) == 0, "exhausted string returns NULL");
    check(next(0) == 0, "stays exhausted");
    put_text(f.runtime.memory, data + 0x200, "  -42x");
    check(f.call("atoi", {data + 0x200}).stop == atlan::Stop::returned && static_cast<std::int32_t>(f.r0()) == -42, "atoi");
    put_text(f.runtime.memory, data + 0x200, "abc");
    check(f.call("atoi", {data + 0x200}).stop == atlan::Stop::returned && f.r0() == 0, "atoi without digits");
    put_text(f.runtime.memory, data + 0x300, "HeLLo");
    put_text(f.runtime.memory, data + 0x340, "hello!");
    check(f.call("strcasecmp", {data + 0x300, data + 0x340}).stop == atlan::Stop::returned && static_cast<std::int32_t>(f.r0()) < 0, "strcasecmp orders by length");
    check(f.call("strncasecmp", {data + 0x300, data + 0x340, 5}).stop == atlan::Stop::returned && f.r0() == 0, "strncasecmp folds case");
    put_text(f.runtime.memory, data + 0x100, ",,");
    check(next(data + 0x100) == 0, "only delimiters returns NULL");
}
void libc_extras() {
    Fixture f;
    put_text(f.runtime.memory, data, "find the needle here");
    put_text(f.runtime.memory, data + 0x40, "needle");
    check(f.call("strstr", {data, data + 0x40}).stop == atlan::Stop::returned && f.r0() == data + 9, "strstr");
    put_text(f.runtime.memory, data + 0x80, "  -123abc");
    check(f.call("strtol", {data + 0x80, data + 0x100, 10}).stop == atlan::Stop::returned && static_cast<std::int32_t>(f.r0()) == -123 &&
          get32(f.runtime.memory, data + 0x100) == data + 0x80 + 6, "strtol value and end pointer");
    check(f.call("sqrt", {0, 0x40100000}).stop == atlan::Stop::returned && f.r0() == 0 && f.runtime.cpu.registers[1] == 0x40000000, "sqrt(4.0) == 2.0 in r0:r1");
    put_text(f.runtime.memory, data + 0x200, "a,b;;c");
    put_text(f.runtime.memory, data + 0x240, ",;");
    std::string tokens;
    for (std::uint32_t s = data + 0x200;; s = 0) {
        check(f.call("strtok", {s, data + 0x240}).stop == atlan::Stop::returned, "strtok call");
        if (f.r0() == 0) break;
        tokens += text_at(f.runtime.memory, f.r0()) + "|";
    }
    check(tokens == "a|b|c|", "strtok tokens");
    put_text(f.runtime.memory, data + 0x500, "DIR/A.TXT");
    put_text(f.runtime.memory, data + 0x540, "r");
    check(f.call("fopen", {data + 0x500, data + 0x540}).stop == atlan::Stop::returned && f.r0() != 0, "fopen matches names case-insensitively");
    put_text(f.runtime.memory, data + 0x500, "DIR/missing.txt");
    check(f.call("fopen", {data + 0x500, data + 0x540}).stop == atlan::Stop::returned && f.r0() == 0, "a missing file still fails");
    // A file created through the services after its directory was listed is found case-insensitively too.
    put_text(f.runtime.memory, data + 0x500, "DIR/New.txt");
    put_text(f.runtime.memory, data + 0x540, "w");
    check(f.call("fopen", {data + 0x500, data + 0x540}).stop == atlan::Stop::returned && f.r0() != 0, "fopen creates a file");
    put_text(f.runtime.memory, data + 0x500, "dir/NEW.TXT");
    put_text(f.runtime.memory, data + 0x540, "r");
    check(f.call("fopen", {data + 0x500, data + 0x540}).stop == atlan::Stop::returned && f.r0() != 0, "a created file is found case-insensitively");
    // qsort calls back into a guest comparator (here a host service standing in for it).
    f.dispatcher.bind("cmp", [&f](atlan::ImportCall& c) -> std::optional<std::string> {
        const auto a = static_cast<std::int32_t>(get32(f.runtime.memory, c.arg(0))), b = static_cast<std::int32_t>(get32(f.runtime.memory, c.arg(1)));
        c.set_result(static_cast<std::uint32_t>(a < b ? -1 : a > b ? 1 : 0));
        return std::nullopt;
    });
    const std::int32_t unsorted[] = {5, -2, 9, 0, 3, 3, -7};
    for (unsigned i = 0; i < 7; ++i) put32(f.runtime.memory, data + 0x400 + 4 * i, static_cast<std::uint32_t>(unsorted[i]));
    f.runtime.cpu.registers[13] = stack;
    check(f.call("qsort", {data + 0x400, 7, 4, 0xf00000c0}).stop == atlan::Stop::returned, "qsort returns");
    const std::int32_t expected[] = {-7, -2, 0, 3, 3, 5, 9};
    bool ordered = true;
    for (unsigned i = 0; i < 7; ++i) ordered &= static_cast<std::int32_t>(get32(f.runtime.memory, data + 0x400 + 4 * i)) == expected[i];
    check(ordered, "qsort orders the array through the comparator");
    // A guest comparator keeps running when the caller's budget runs out during the sort: a callback cannot be
    // suspended, so it has its own bound and its instructions are charged to the caller afterwards.
    {
        constexpr std::uint32_t code = 0x40000;
        const std::uint32_t words[] = {0xe5900000, 0xe5911000, 0xe0400001, 0xe12fff1e};  // ldr r0,[r0]; ldr r1,[r1]; sub r0,r0,r1; bx lr
        std::array<std::byte, 16> bytes{};
        std::memcpy(bytes.data(), words, sizeof words);
        f.runtime.memory.map(code, 0x1000, atlan::Permissions::read_execute, bytes);
        for (unsigned i = 0; i < 7; ++i) put32(f.runtime.memory, data + 0x400 + 4 * i, static_cast<std::uint32_t>(unsorted[i]));
        f.runtime.cpu.registers[13] = stack;
        const std::vector<std::uint32_t> args{data + 0x400, 7, 4, code};
        const auto sorted = f.dispatcher.call(f.runtime, trap("qsort"), args, stack, sentinel, 20);
        check(sorted.stop == atlan::Stop::budget_exhausted || sorted.stop == atlan::Stop::returned, "qsort with a guest comparator outlasts a small budget");
        bool guest_ordered = true;
        for (unsigned i = 0; i < 7; ++i) guest_ordered &= static_cast<std::int32_t>(get32(f.runtime.memory, data + 0x400 + 4 * i)) == expected[i];
        check(guest_ordered, "qsort orders the array through a guest comparator past the caller's budget");
        check(f.dispatcher.remaining_budget() == 0, "the comparator's instructions are charged to the caller");
    }
    // setjmp saves callee-saved state; longjmp resumes at the setjmp return address with the given value.
    f.runtime.cpu.registers[4] = 0x1234; f.runtime.cpu.registers[14] = 0xffff0000; f.runtime.cpu.registers[13] = stack;
    check(f.call("setjmp", {data + 0x300}).stop == atlan::Stop::returned && f.r0() == 0, "setjmp returns 0");
    check(f.call("longjmp", {data + 0x300, 7}).stop == atlan::Stop::returned && f.r0() == 7, "longjmp returns the value at the saved address");
    check(f.call("longjmp", {data + 0x300, 0}).stop == atlan::Stop::returned && f.r0() == 1, "longjmp(0) returns 1");
    check(f.call("longjmp", {data + 0x340, 1}).stop == atlan::Stop::service_error, "longjmp with an unfilled buffer stops");
}
void file_mapping() {
    Fixture f;
    f.arena.set_host_descriptors([&f](std::uint32_t fd) { return f.stdio.host_descriptor(fd); });
    std::string content(10000, 'x');
    for (std::size_t i = 0; i < content.size(); ++i) content[i] = static_cast<char>('a' + i % 26);
    { std::ofstream out(f.root / "big.bin", std::ios::binary); out << content; }
    put_text(f.runtime.memory, data, "big.bin");
    check(f.call("open", {data, 0}).stop == atlan::Stop::returned && f.r0() >= 3 && f.r0() < 100, "open the file to map");
    const auto fd = f.r0();
    check(f.call("mmap", {0, 12288, 1, 0x2, fd, 0}).stop == atlan::Stop::returned && f.r0() >= 0x90000000 && f.r0() != 0xffffffff, "file mmap");
    const auto at = f.r0();
    std::vector<std::byte> got(12288);
    check(f.runtime.memory.read(at, got), "mapping is readable");
    bool same = true;
    for (std::size_t i = 0; i < 10000; ++i) same &= static_cast<char>(got[i]) == content[i];
    for (std::size_t i = 10000; i < 12288; ++i) same &= got[i] == std::byte{0};
    check(same, "file content, then zeros past the end");
    const std::array<std::byte, 1> one{std::byte{1}};
    check(!f.runtime.memory.write(at, one), "read-only mapping rejects writes");
    check(f.call("mmap", {0, 8192, 3, 0x2, fd, 4096}).stop == atlan::Stop::returned && f.r0() != 0xffffffff, "writable private mapping at an offset");
    const auto rw = f.r0();
    check(f.runtime.memory.write(rw + 10, one), "private mapping accepts writes");
    std::string on_disk(10000, '\0');
    { std::ifstream in(f.root / "big.bin", std::ios::binary); in.read(on_disk.data(), 10000); }
    check(on_disk == content, "writes never reach the file");
    std::array<std::byte, 1> back{};
    check(f.runtime.memory.read(rw + 11, back) && static_cast<char>(back[0]) == content[4096 + 11], "offset mapping reads the right bytes");
    check(f.call("munmap", {at, 12288}).stop == atlan::Stop::returned, "munmap of a file mapping");
    check(f.call("mmap", {0, 4096, 1, 0x2, 99, 0}).stop == atlan::Stop::returned && f.r0() == 0xffffffff, "bad fd gives MAP_FAILED");
    check(f.call("mmap", {0, 4096, 1, 0x1, fd, 0}).stop == atlan::Stop::returned && f.r0() != 0xffffffff, "MAP_SHARED read-only mapping");
    check(f.call("mmap", {0, 4096, 3, 0x1, fd, 0}).stop == atlan::Stop::service_error, "MAP_SHARED write mapping of a read-only fd stops");
    check(f.call("open", {data, 2}).stop == atlan::Stop::returned && f.r0() >= 3, "open the file read-write");
    const auto wfd = f.r0();
    check(f.call("mmap", {0, 4096, 3, 0x1, wfd, 0}).stop == atlan::Stop::returned && f.r0() != 0xffffffff, "MAP_SHARED write mapping of a writable fd");
    const auto shared = f.r0();
    const std::array<std::byte, 1> mark{std::byte{'#'}};
    check(f.runtime.memory.write(shared + 5, mark), "shared mapping accepts writes");
    std::string after(10000, '\0');
    { std::ifstream in(f.root / "big.bin", std::ios::binary); in.read(after.data(), 10000); }
    check(after[5] == '#' && after[6] == content[6], "shared writes reach the file");
}
void jni_strings() {
    Fixture f;
    atlan::JavaBootstrap java(f.runtime, f.dispatcher, {});
    const auto slot = [&](std::size_t index) { return get32(f.runtime.memory, get32(f.runtime.memory, java.env()) + 4 * static_cast<std::uint32_t>(index)); };
    const auto s = java.new_string("héllo");
    check(s != 0 && java.string_text(s) == "héllo" && !java.string_text(0x1234), "host string");
    auto r = f.dispatcher.call(f.runtime, slot(169), std::vector<std::uint32_t>{java.env(), s, data}, stack, sentinel, 1000);
    check(r.stop == atlan::Stop::returned, "GetStringUTFChars");
    const auto copy = f.r0();
    check(text_at(f.runtime.memory, copy) == "héllo" && get32(f.runtime.memory, data) % 256 == 1, "copy is NUL terminated and isCopy set");
    check(f.dispatcher.call(f.runtime, slot(168), std::vector<std::uint32_t>{java.env(), s}, stack, sentinel, 1000).stop == atlan::Stop::returned && f.r0() == 6, "GetStringUTFLength counts bytes");
    check(f.dispatcher.call(f.runtime, slot(170), std::vector<std::uint32_t>{java.env(), s, copy}, stack, sentinel, 1000).stop == atlan::Stop::returned, "ReleaseStringUTFChars");
    check(f.dispatcher.call(f.runtime, slot(170), std::vector<std::uint32_t>{java.env(), s, copy}, stack, sentinel, 1000).stop == atlan::Stop::service_error, "double release is rejected");
    put_text(f.runtime.memory, data + 0x40, "from guest");
    check(f.dispatcher.call(f.runtime, slot(167), std::vector<std::uint32_t>{java.env(), data + 0x40}, stack, sentinel, 1000).stop == atlan::Stop::returned &&
          java.string_text(f.r0()) == "from guest", "NewStringUTF");
    check(f.dispatcher.call(f.runtime, slot(169), std::vector<std::uint32_t>{java.env(), 0xe0000abc, 0}, stack, sentinel, 1000).stop == atlan::Stop::service_error, "non-string is rejected");
}
void jni_arrays_and_misc() {
    Fixture f;
    atlan::JavaBootstrap java(f.runtime, f.dispatcher, {});
    const auto slot = [&](std::size_t index) { return get32(f.runtime.memory, get32(f.runtime.memory, java.env()) + 4 * static_cast<std::uint32_t>(index)); };
    const auto invoke = [&](std::size_t index, std::vector<std::uint32_t> args) { args.insert(args.begin(), java.env()); return f.dispatcher.call(f.runtime, slot(index), args, stack, sentinel, 1000); };
    check(invoke(4, {}).stop == atlan::Stop::returned && f.r0() == 0x10006, "GetVersion");
    for (std::size_t i : {228u, 15u, 17u}) check(invoke(i, {}).stop == atlan::Stop::returned && f.r0() == 0, "no exception is ever pending");
    const auto arr = java.new_byte_array({1, 2, 3, 4});
    check(arr != 0 && java.byte_array(arr)->size() == 4 && !java.byte_array(0x1234), "host byte array");
    check(invoke(171, {arr}).stop == atlan::Stop::returned && f.r0() == 4, "GetArrayLength");
    check(invoke(171, {0xe6000abc}).stop == atlan::Stop::service_error, "GetArrayLength rejects a forged array");
    check(invoke(199 + 1, {arr, 1, 2, data}).stop == atlan::Stop::returned && get32(f.runtime.memory, data) % 65536 == 0x0302, "GetByteArrayRegion");
    check(invoke(199 + 1, {arr, 3, 2, data}).stop == atlan::Stop::service_error, "region past the end stops");
    put32(f.runtime.memory, data + 0x10, 0x0a0b0c0d);
    check(invoke(207 + 1, {arr, 0, 4, data + 0x10}).stop == atlan::Stop::returned && (*java.byte_array(arr))[0] == 0x0d && (*java.byte_array(arr))[3] == 0x0a, "SetByteArrayRegion");
    check(invoke(184, {arr, data + 0x20}).stop == atlan::Stop::returned, "GetByteArrayElements");
    const auto copy = f.r0();
    check(copy >= 0xf4000000 && get32(f.runtime.memory, copy) == 0x0a0b0c0d, "elements copy has the contents");
    put32(f.runtime.memory, copy, 0x01010101);
    check(invoke(192, {arr, copy, 2}).stop == atlan::Stop::returned && (*java.byte_array(arr))[0] == 0x0d, "JNI_ABORT discards changes");
    check(invoke(184, {arr, 0}).stop == atlan::Stop::returned, "second elements copy");
    const auto copy2 = f.r0();
    put32(f.runtime.memory, copy2, 0x05050505);
    check(invoke(192, {arr, copy2, 0}).stop == atlan::Stop::returned && (*java.byte_array(arr))[0] == 5, "mode 0 copies back");
    check(invoke(192, {arr, copy2, 0}).stop == atlan::Stop::service_error, "double release is rejected");
    check(invoke(176, {3}).stop == atlan::Stop::returned && java.byte_array(f.r0())->size() == 3, "NewByteArray");
    check(invoke(176, {0xffffffff}).stop == atlan::Stop::service_error, "negative length is rejected");
    // GetObjectClass and CallNonvirtual*.
    put_text(f.runtime.memory, data + 0x100, "pkg/Base");
    put_text(f.runtime.memory, data + 0x140, "ping");
    put_text(f.runtime.memory, data + 0x160, "(I)I");
    const auto cls = java.class_handle("pkg/Base");
    const auto obj = java.new_object(cls);
    check(invoke(31, {obj}).stop == atlan::Stop::returned && f.r0() == cls, "GetObjectClass");
    check(invoke(33, {cls, data + 0x140, data + 0x160}).stop == atlan::Stop::returned, "method id");
    const auto mid = f.r0();
    java.register_method("pkg/Base", "ping", "(I)I", [](atlan::JavaBootstrap::MethodCall& c) -> std::optional<std::string> { c.result = c.args.at(0) * 2; return std::nullopt; });
    check(invoke(49, {obj, mid, 21}).stop == atlan::Stop::returned && f.r0() == 42, "CallIntMethod");
    check(invoke(21, {obj}).stop == atlan::Stop::returned, "NewGlobalRef of an object");
    const auto global_obj = f.r0();
    check(invoke(49, {global_obj, mid, 21}).stop == atlan::Stop::returned && f.r0() == 42, "CallIntMethod accepts a global reference receiver");
    check(invoke(79, {obj, cls, mid, 8}).stop == atlan::Stop::returned && f.r0() == 16, "CallNonvirtualIntMethod passes clazz before the id");
    check(invoke(79, {obj, java.class_handle("pkg/Other"), mid, 8}).stop == atlan::Stop::service_error, "CallNonvirtual class mismatch stops");
}
void jni_korean_strings() {
    Fixture f;
    atlan::JavaBootstrap::Config config;
    config.guest_chars = atlan::JavaBootstrap::Config::CharEncoding::cp949;
    atlan::JavaBootstrap java(f.runtime, f.dispatcher, config);
    const auto slot = [&](std::size_t index) { return get32(f.runtime.memory, get32(f.runtime.memory, java.env()) + 4 * static_cast<std::uint32_t>(index)); };
    const auto call = [&](std::size_t index, std::vector<std::uint32_t> args) {
        return f.dispatcher.call(f.runtime, slot(index), args, stack, sentinel, 1000);
    };
    put_text(f.runtime.memory, data, "A\xC7\xD1\xB1\xDB");  // "A한글" in CP949
    check(call(167, {java.env(), data}).stop == atlan::Stop::returned && java.string_text(f.r0()) == "A한글", "NewStringUTF decodes CP949");
    const auto s = java.new_string("A한글");
    check(call(168, {java.env(), s}).stop == atlan::Stop::returned && f.r0() == 5, "GetStringUTFLength counts CP949 bytes");
    check(call(169, {java.env(), s, 0}).stop == atlan::Stop::returned, "GetStringUTFChars");
    const auto copy = f.r0();
    check(text_at(f.runtime.memory, copy) == "A\xC7\xD1\xB1\xDB", "copy is CP949");
    check(call(170, {java.env(), s, copy}).stop == atlan::Stop::returned, "release");
    // UTF-16 side is independent of the guest char encoding.
    check(call(164, {java.env(), s}).stop == atlan::Stop::returned && f.r0() == 3, "GetStringLength counts UTF-16 units");
    check(call(165, {java.env(), s, 0}).stop == atlan::Stop::returned, "GetStringChars");
    const auto wide = f.r0();
    check(get32(f.runtime.memory, wide) == 0xD55C0041 && get32(f.runtime.memory, wide + 4) % 0x10000 == 0xAE00, "UTF-16 content");
    check(call(166, {java.env(), s, wide}).stop == atlan::Stop::returned, "ReleaseStringChars");
    const std::vector<std::byte> utf16{std::byte{0x5C}, std::byte{0xD5}, std::byte{0x00}, std::byte{0xAE}};
    check(f.runtime.memory.write(data + 0x40, utf16), "write UTF-16 input");
    check(call(163, {java.env(), data + 0x40, 2}).stop == atlan::Stop::returned && java.string_text(f.r0()) == "한글", "NewString from UTF-16");
    // Unconvertible text is replaced, not fatal.
    put_text(f.runtime.memory, data + 0x80, "\xB0");
    check(call(167, {java.env(), data + 0x80}).stop == atlan::Stop::returned && java.string_text(f.r0()) == "\xEF\xBF\xBD", "malformed CP949 becomes U+FFFD");
    put_text(f.runtime.memory, data + 0xc0, "plain");
    check(call(167, {java.env(), data + 0xc0}).stop == atlan::Stop::returned, "ASCII string");
    put_text(f.runtime.memory, data + 0x100, "\xED\x95\x9C");  // "한" in UTF-8
    check(call(167, {java.env(), data + 0x100}).stop == atlan::Stop::returned, "UTF-8 Hangul");
    const auto& stats = java.string_input_stats();
    check(stats.calls == 4 && stats.ascii == 1 && stats.utf8 == 1 && stats.cp949_only == 1 && stats.neither == 1,
          "input statistics classify CP949, UTF-8, ASCII and malformed input");
}
}
// memcmp, strcmp, strncmp, memset, memcpy, memmove and strlen run inline from the block cache; the step path runs
// their services. Both give the same results, the inline calls are counted with the imports, and a call that
// faults is left to the service, which reports it without having changed memory.
void inline_fast_paths() {
    for (const bool cached : {true, false}) {
        Fixture f;
        f.dispatcher.set_block_cache(cached);
        put_text(f.runtime.memory, data, "abcdef");
        put_text(f.runtime.memory, data + 0x10, "abcxyz");
        check(f.call("memcmp", {data, data + 0x10, 6}).stop == atlan::Stop::returned && f.r0() == std::uint32_t('d' - 'x'), "memcmp difference");
        check(f.call("memcmp", {data, data + 0x10, 3}).stop == atlan::Stop::returned && f.r0() == 0, "memcmp equal prefix");
        check(f.call("strncmp", {data, data + 0x10, 4}).stop == atlan::Stop::returned && f.r0() == std::uint32_t('d' - 'x'), "strncmp difference");
        check(f.call("strcmp", {data, data}).stop == atlan::Stop::returned && f.r0() == 0, "strcmp equal");
        check(f.call("strlen", {data}).stop == atlan::Stop::returned && f.r0() == 6, "strlen");
        check(f.call("memset", {data + 0x20, 0x41, 5}).stop == atlan::Stop::returned && f.r0() == data + 0x20, "memset returns dest");
        check(f.call("memcpy", {data + 0x40, data + 0x20, 5}).stop == atlan::Stop::returned && f.r0() == data + 0x40, "memcpy returns dest");
        check(text_at(f.runtime.memory, data + 0x40) == "AAAAA", "memcpy copies");
        check(f.call("memmove", {data + 0x41, data + 0x40, 5}).stop == atlan::Stop::returned && text_at(f.runtime.memory, data + 0x40) == "AAAAAA",
              "memmove overlap");
        // A copy that runs off the mapping: the service reports it and nothing was written.
        put_text(f.runtime.memory, data + 0x3ff0, "keep");
        check(f.call("memcpy", {data + 0x3ff0, data, 0x20}).stop == atlan::Stop::service_error, "memcpy past the mapping fails");
        check(text_at(f.runtime.memory, data + 0x3ff0) == "keep", "failed memcpy wrote nothing");
        check(f.call("memcmp", {data, 0x9000000, 4}).stop == atlan::Stop::service_error, "memcmp of unmapped memory fails");
        const auto& counts = f.dispatcher.import_counts();
        check(counts.count("memcmp") != 0 && counts.at("memcmp") == 3, "memcmp calls counted");
        check(counts.count("memcpy") != 0 && counts.at("memcpy") == 2, "memcpy calls counted");
    }
}
int main() {
    try {
        strings();
        inline_fast_paths();
        printf_family();
        mapping();
        files();
        path_mapping();
        directories_and_descriptors();
        properties_and_strings();
        tokenizing();
        file_mapping();
        libc_extras();
        jni_strings();
        jni_arrays_and_misc();
        jni_korean_strings();
    } catch (const std::exception& error) {
        std::cerr << "service test failed: " << error.what() << '\n';
        return 1;
    }
    std::cout << "service checks passed: " << checks << '\n';
}
