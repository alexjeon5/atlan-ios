// SPDX-License-Identifier: GPL-2.0-or-later
#include "atlan/abi.hpp"
#include "atlan/jni.hpp"
#include "atlan/data_symbols.hpp"
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
                      {"fsync", 0xf000008c}, {"ftruncate", 0xf0000090}};
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
}
}
int main() {
    try {
        strings();
        printf_family();
        mapping();
        files();
        directories_and_descriptors();
        properties_and_strings();
        jni_strings();
        jni_arrays_and_misc();
        jni_korean_strings();
    } catch (const std::exception& error) {
        std::cerr << "service test failed: " << error.what() << '\n';
        return 1;
    }
    std::cout << "service checks passed: " << checks << '\n';
}
