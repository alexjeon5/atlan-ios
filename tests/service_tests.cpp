// SPDX-License-Identifier: GPL-2.0-or-later
#include "atlan/abi.hpp"
#include "atlan/jni.hpp"
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
                      {"feof", 0xf000004c}};
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
}
int main() {
    try {
        strings();
        printf_family();
        mapping();
        files();
        properties_and_strings();
        jni_strings();
        jni_arrays_and_misc();
    } catch (const std::exception& error) {
        std::cerr << "service test failed: " << error.what() << '\n';
        return 1;
    }
    std::cout << "service checks passed: " << checks << '\n';
}
