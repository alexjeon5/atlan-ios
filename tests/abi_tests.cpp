// SPDX-License-Identifier: GPL-2.0-or-later
#include "atlan/abi.hpp"
#include "atlan/android_log.hpp"
#include "atlan/jni.hpp"
#include <array>
#include <bit>
#include <cstring>
#include <iostream>
#include <stdexcept>

namespace {
unsigned checks{};
void check(bool result, const char* message) {
    ++checks;
    if (!result) throw std::runtime_error(message);
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
void put_text(atlan::Memory& memory, std::uint32_t at, const char* text) {
    const auto size = std::strlen(text) + 1;
    check(memory.write(at, std::span<const std::byte>(reinterpret_cast<const std::byte*>(text), size)), "write text");
}
void put64(atlan::Memory& memory, std::uint32_t at, std::uint64_t value) {
    put32(memory, at, static_cast<std::uint32_t>(value));
    put32(memory, at + 4, static_cast<std::uint32_t>(value >> 32));
}
constexpr std::uint32_t code = 0x1000, stack = 0x80000, sentinel = 0xffff0000, data = 0x4000;
constexpr std::uint32_t log_print = 0xf0000000, log_vprint = 0xf0000004, log_write = 0xf0000008;

struct Fixture {
    atlan::Runtime runtime;
    atlan::ImportDispatcher dispatcher;
    std::vector<atlan::LogEntry> entries;
    atlan::AndroidLog log{[this](const atlan::LogEntry& e) { entries.push_back(e); }};
    Fixture() {
        runtime.memory.map(code, 0x2000, atlan::Permissions::read_write);
        runtime.memory.map(data, 0x2000, atlan::Permissions::read_write);
        runtime.memory.map(stack - 0x1000, 0x1000, atlan::Permissions::read_write);
        const std::uint32_t sum6[] = {0xe0800001, 0xe0800002, 0xe0800003, 0xe59d1000, 0xe0800001, 0xe59d1004, 0xe0800001, 0xe12fff1e};
        for (unsigned i = 0; i < 8; ++i) put32(runtime.memory, 0x1000 + 4 * i, sum6[i]);
        // JNI_OnLoad(vm, reserved): vm->GetEnv(&env, 0x10006); returns the version, or -1.
        const std::uint32_t on_load[] = {0xe92d4010, 0xe24dd008, 0xe5903000, 0xe5933018, 0xe1a0100d, 0xe59f2028,
                                         0xe12fff33, 0xe3500000, 0x1a000004, 0xe59d0000, 0xe3500000, 0x0a000001,
                                         0xe59f000c, 0xea000000, 0xe3e00000, 0xe28dd008, 0xe8bd8010, 0x00010006};
        for (unsigned i = 0; i < 18; ++i) put32(runtime.memory, 0x1100 + 4 * i, on_load[i]);
        // env->NewStringUTF (slot 167, not implemented): ldr r1,[r0]; ldr r1,[r1,#668]; bx r1
        const std::uint32_t find_class[] = {0xe5901000, 0xe591129c, 0xe12fff11};
        for (unsigned i = 0; i < 3; ++i) put32(runtime.memory, 0x1300 + 4 * i, find_class[i]);
        runtime.memory.protect(code, 0x2000, atlan::Permissions::read_execute);
        atlan::elf::Image image;
        image.trapped_imports = {{"__android_log_print", log_print, 2}, {"__android_log_vprint", log_vprint, 2},
                                 {"__android_log_write", log_write, 2}};
        dispatcher.attach(image);
        log.install(dispatcher);
    }
    atlan::CallOutcome call(std::uint32_t target, std::initializer_list<std::uint32_t> args, std::uint64_t budget = 10000) {
        const std::vector<std::uint32_t> values(args);
        return dispatcher.call(runtime, target, values, stack, sentinel, budget);
    }
};

void stack_arguments() {
    Fixture f;
    const std::uint32_t args[] = {1, 2, 3, 4, 50, 600};
    auto r = f.runtime.call(0x1000, args, stack, sentinel, 100);
    check(r.event == atlan::Event::function_return && f.runtime.cpu.registers[0] == 660, "six-argument call sums register and stack words");
    check(f.runtime.cpu.registers[13] == stack - 8, "sp is 8-byte aligned below the stack words");
    check(get32(f.runtime.memory, stack - 8) == 50 && get32(f.runtime.memory, stack - 4) == 600, "stack words are consecutive from sp");
    const auto out = f.call(0x1000, {1, 2, 3, 4, 5, 6});
    check(out.stop == atlan::Stop::returned && f.runtime.cpu.registers[0] == 21, "dispatcher call passes stack arguments");
    std::vector<std::uint32_t> many(atlan::Runtime::max_call_arguments + 1, 1);
    bool threw = false;
    try { (void)f.dispatcher.call(f.runtime, 0x1000, many, stack, sentinel, 100); } catch (const std::invalid_argument&) { threw = true; }
    check(threw, "too many arguments are rejected");
    r = f.runtime.call(0x1000, args, 0x90000000, sentinel, 100);
    check(r.event == atlan::Event::memory_fault && r.access == atlan::MemoryAccess::write, "unwritable stack frame faults before running");
}
void log_print_formats() {
    Fixture f;
    put_text(f.runtime.memory, data + 0x100, "TAG");
    put_text(f.runtime.memory, data + 0x200, "%d %s %x %lld %.2f %c %%");
    put_text(f.runtime.memory, data + 0x300, "hi");
    // words: prio, tag, fmt, int, [stack] str, hex, ll(lo,hi) at even slot 6, double at slot 8, char
    const auto pi = std::bit_cast<std::uint64_t>(3.14159);
    auto r = f.call(log_print, {4, data + 0x100, data + 0x200, static_cast<std::uint32_t>(-5), data + 0x300, 0xbeef,
                                0, 0x00000001, static_cast<std::uint32_t>(pi), static_cast<std::uint32_t>(pi >> 32), 'Z'});
    // Slot 6 holds the low half of the long long (0), slot 7 the high half (1).
    check(r.stop == atlan::Stop::returned, "log_print returns");
    check(f.entries.size() == 1 && f.entries[0].priority == 4 && f.entries[0].tag == "TAG", "priority and tag are delivered");
    check(f.entries[0].message == "-5 hi beef 4294967296 3.14 Z %", "int, string, hex, 64-bit, double and char are read in order");
    check(f.runtime.cpu.registers[0] == f.entries[0].message.size(), "result is the message length");
}
void log_print_alignment() {
    Fixture f;
    put_text(f.runtime.memory, data + 0x100, "T");
    put_text(f.runtime.memory, data + 0x200, "%d %d %lld %f");
    // slots: 3:int 4:int 5:pad 6,7:ll 8,9:double
    const auto bits = std::bit_cast<std::uint64_t>(2.5);
    auto r = f.call(log_print, {3, data + 0x100, data + 0x200, 1, 2, 0xdead, 0xfffffffe, 0xffffffff,
                                static_cast<std::uint32_t>(bits), static_cast<std::uint32_t>(bits >> 32)});
    check(r.stop == atlan::Stop::returned && f.entries.size() == 1, "aligned varargs call returns");
    check(f.entries[0].message == "1 2 -2 2.500000", "64-bit values skip to even slots and keep sign");
    // 64-bit argument right after r3: slot 3 odd is skipped, so the value is read from slots 4,5.
    put_text(f.runtime.memory, data + 0x200, "%lld");
    r = f.call(log_print, {3, data + 0x100, data + 0x200, 0xbad, 7, 0});
    check(r.stop == atlan::Stop::returned && f.entries.back().message == "7", "doubleword never straddles r3 and the stack");
}
void log_vprint_and_write() {
    Fixture f;
    put_text(f.runtime.memory, data + 0x100, "V");
    put_text(f.runtime.memory, data + 0x200, "%d|%lld|%s");
    put_text(f.runtime.memory, data + 0x300, "str");
    put32(f.runtime.memory, data + 0x500, 7);
    put64(f.runtime.memory, data + 0x508, 0x100000002ULL);  // aligned up from +4
    put32(f.runtime.memory, data + 0x510, data + 0x300);
    auto r = f.call(log_vprint, {5, data + 0x100, data + 0x200, data + 0x500});
    check(r.stop == atlan::Stop::returned && f.entries.back().message == "7|4294967298|str", "vprint reads the va_list pointer");
    put_text(f.runtime.memory, data + 0x600, "plain %d");
    r = f.call(log_write, {6, data + 0x100, data + 0x600});
    check(r.stop == atlan::Stop::returned && f.entries.back().message == "plain %d" && f.entries.back().priority == 6,
          "write logs text without formatting");
    r = f.call(log_write, {6, 0, data + 0x600});
    check(r.stop == atlan::Stop::returned && f.entries.back().tag.empty(), "null tag becomes empty");
}
void log_failures() {
    Fixture f;
    put_text(f.runtime.memory, data + 0x100, "T");
    const auto fails = [&](const char* format, std::initializer_list<std::uint32_t> extra, const char* what) {
        put_text(f.runtime.memory, data + 0x200, format);
        std::vector<std::uint32_t> args{3, data + 0x100, data + 0x200};
        args.insert(args.end(), extra);
        const auto before = f.entries.size();
        const auto r = f.dispatcher.call(f.runtime, log_print, args, stack, sentinel, 1000);
        check(r.stop == atlan::Stop::service_error && f.entries.size() == before, what);
    };
    fails("%n", {data}, "%n is rejected");
    fails("%q", {1}, "unknown conversion is rejected");
    fails("%s", {0x9000000}, "unreadable string argument is rejected");
    fails("%", {}, "format ending in a conversion is rejected");
    fails("%Lf", {1}, "long double is rejected");
    fails("%d %d %d %d %d %d", {1, 2, 3, 4, 5}, "reading past the readable stack fails explicitly");
    check(f.call(log_print, {3, 0, data + 0x200}).stop == atlan::Stop::service_error, "unreadable tag is rejected") ;
    check(f.call(log_print, {3, data + 0x100, 0}).stop == atlan::Stop::service_error, "null format is rejected");
    check(f.call(log_vprint, {3, data + 0x100, data + 0x200, 0x9000000}).stop == atlan::Stop::service_error, "unmapped va_list is rejected");
}
void jni_bootstrap() {
    Fixture f;
    atlan::JavaBootstrap java(f.runtime, f.dispatcher, {});
    check(atlan::JavaBootstrap::invoke_name(6) == "JavaVM::GetEnv" && atlan::JavaBootstrap::native_name(6) == "JNIEnv::FindClass" &&
          atlan::JavaBootstrap::native_name(4) == "JNIEnv::GetVersion" && atlan::JavaBootstrap::native_name(215) == "JNIEnv::RegisterNatives" &&
          atlan::JavaBootstrap::native_name(232) == "JNIEnv::GetObjectRefType" && atlan::JavaBootstrap::native_name(33) == "JNIEnv::GetMethodID" &&
          atlan::JavaBootstrap::native_name(64) == "JNIEnv::CallNonvirtualObjectMethod" && atlan::JavaBootstrap::native_name(94) == "JNIEnv::GetFieldID" &&
          atlan::JavaBootstrap::native_name(113) == "JNIEnv::GetStaticMethodID" && atlan::JavaBootstrap::native_name(167) == "JNIEnv::NewStringUTF",
          "slot names follow the JNI table order");
    // The tables are read-only guest data.
    check(!f.runtime.memory.write(java.vm(), std::array<std::byte, 4>{}), "JavaVM object is read-only");
    auto r = f.call(0x1100, {java.vm(), 0});
    check(r.stop == atlan::Stop::returned && f.runtime.cpu.registers[0] == 0x10006, "guest JNI_OnLoad obtains the env through GetEnv");
    // Unsupported version: *penv cleared, JNI_EVERSION returned.
    const auto get_env = get32(f.runtime.memory, get32(f.runtime.memory, java.vm()) + 24);
    put32(f.runtime.memory, data, 0x1234);
    r = f.call(get_env, {java.vm(), data, 0x00020000});
    check(r.stop == atlan::Stop::returned && static_cast<std::int32_t>(f.runtime.cpu.registers[0]) == -3 && get32(f.runtime.memory, data) == 0,
          "unsupported JNI version returns JNI_EVERSION and clears the output");
    check(f.call(get_env, {0x1234, data, 0x10006}).stop == atlan::Stop::service_error, "foreign JavaVM pointer is rejected");
    check(f.call(get_env, {java.vm(), 0, 0x10006}).stop == atlan::Stop::service_error, "null output pointer is rejected");
    check(f.call(get_env, {java.vm(), 0x9000000, 0x10006}).stop == atlan::Stop::service_error, "unwritable output pointer is rejected");
    // An unobserved JNI function stops by name; it is never a silent success.
    r = f.call(0x1300, {java.env()});
    check(r.stop == atlan::Stop::unresolved_import && r.import == "JNIEnv::NewStringUTF", "unimplemented JNI function stops by name");
    // Class, member and native-method registration through the env table.
    const auto slot = [&](std::size_t index) { return get32(f.runtime.memory, get32(f.runtime.memory, java.env()) + 4 * static_cast<std::uint32_t>(index)); };
    put_text(f.runtime.memory, data + 0x100, "pkg/Thing");
    put_text(f.runtime.memory, data + 0x180, "run");
    put_text(f.runtime.memory, data + 0x1a0, "(I)V");
    put_text(f.runtime.memory, data + 0x1c0, "count");
    put_text(f.runtime.memory, data + 0x1e0, "I");
    r = f.call(slot(6), {java.env(), data + 0x100});
    check(r.stop == atlan::Stop::returned, "FindClass returns");
    const auto cls = static_cast<std::uint32_t>(f.runtime.cpu.registers[0]);
    check(cls != 0 && f.runtime.memory.translate(cls, atlan::MemoryAccess::read) == nullptr, "class reference is an unmapped opaque handle");
    r = f.call(slot(6), {java.env(), data + 0x100});
    check(f.runtime.cpu.registers[0] == cls && java.classes().size() == 1, "same name yields the same reference");
    check(f.call(slot(6), {java.env(), 0}).stop == atlan::Stop::service_error, "null class name is rejected");
    check(f.call(slot(33), {java.env(), 0x1234, data + 0x180, data + 0x1a0}).stop == atlan::Stop::service_error, "GetMethodID rejects a forged class handle");
    r = f.call(slot(33), {java.env(), cls, data + 0x180, data + 0x1a0});
    const auto method = static_cast<std::uint32_t>(f.runtime.cpu.registers[0]);
    r = f.call(slot(33), {java.env(), cls, data + 0x180, data + 0x1a0});
    check(f.runtime.cpu.registers[0] == method && method != 0 && java.members().size() == 1 &&
          java.members()[0].class_name == "pkg/Thing" && java.members()[0].name == "run" && java.members()[0].signature == "(I)V",
          "GetMethodID records and deduplicates the member");
    check(f.call(slot(94), {java.env(), cls, data + 0x1c0, data + 0x1e0}).stop == atlan::Stop::returned &&
          f.runtime.cpu.registers[0] != method && java.members().size() == 2, "GetFieldID yields a distinct id");
    // JNINativeMethod table: two entries, the second invalid -> nothing registered.
    put32(f.runtime.memory, data + 0x200, data + 0x180); put32(f.runtime.memory, data + 0x204, data + 0x1a0); put32(f.runtime.memory, data + 0x208, 0x1001);
    put32(f.runtime.memory, data + 0x20c, data + 0x1c0); put32(f.runtime.memory, data + 0x210, data + 0x1e0); put32(f.runtime.memory, data + 0x214, 0);
    check(f.call(slot(215), {java.env(), cls, data + 0x200, 2}).stop == atlan::Stop::service_error && java.native_methods().empty(),
          "RegisterNatives is all-or-nothing");
    put32(f.runtime.memory, data + 0x214, 0x1100);
    r = f.call(slot(215), {java.env(), cls, data + 0x200, 2});
    check(r.stop == atlan::Stop::returned && f.runtime.cpu.registers[0] == 0 && java.native_methods().size() == 2 &&
          java.native_methods()[0].address == 0x1001 && java.native_methods()[1].name == "count" && java.native_methods()[1].class_name == "pkg/Thing",
          "RegisterNatives records guest function addresses");
    // Minimal String instance, global references and local deletion.
    put_text(f.runtime.memory, data + 0x240, "java/lang/String");
    put_text(f.runtime.memory, data + 0x280, "<init>");
    put_text(f.runtime.memory, data + 0x2a0, "()V");
    put_text(f.runtime.memory, data + 0x2c0, "(I)V");
    check(f.call(slot(6), {java.env(), data + 0x240}).stop == atlan::Stop::returned, "FindClass String");
    const auto str_cls = static_cast<std::uint32_t>(f.runtime.cpu.registers[0]);
    check(f.call(slot(33), {java.env(), str_cls, data + 0x280, data + 0x2a0}).stop == atlan::Stop::returned, "String ctor id");
    const auto ctor = static_cast<std::uint32_t>(f.runtime.cpu.registers[0]);
    check(f.call(slot(33), {java.env(), str_cls, data + 0x280, data + 0x2c0}).stop == atlan::Stop::returned, "other ctor id");
    const auto other_ctor = static_cast<std::uint32_t>(f.runtime.cpu.registers[0]);
    for (std::size_t index : {28u, 29u, 30u}) {
        r = f.call(slot(index), {java.env(), str_cls, ctor, 0});
        check(r.stop == atlan::Stop::returned && f.runtime.cpu.registers[0] != 0, "NewObject* creates a String");
    }
    check(java.objects().size() == 3 && java.objects()[0] == "java/lang/String", "objects are recorded");
    const auto obj = static_cast<std::uint32_t>(f.runtime.cpu.registers[0]);
    check(f.call(slot(29), {java.env(), str_cls, other_ctor, 0}).stop == atlan::Stop::service_error, "unmodelled constructor stops");
    check(f.call(slot(29), {java.env(), cls, method, 0}).stop == atlan::Stop::service_error, "non-constructor id stops");
    check(f.call(slot(21), {java.env(), obj}).stop == atlan::Stop::returned, "NewGlobalRef");
    const auto global = static_cast<std::uint32_t>(f.runtime.cpu.registers[0]);
    check(global != obj && global != 0 && java.live_global_refs() == 1, "global reference is distinct and counted");
    check(f.call(slot(21), {java.env(), 0}).stop == atlan::Stop::returned && f.runtime.cpu.registers[0] == 0, "NewGlobalRef(NULL) is NULL");
    check(f.call(slot(21), {java.env(), 0xe0001000}).stop == atlan::Stop::service_error, "forged reference rejected");
    check(f.call(slot(23), {java.env(), obj}).stop == atlan::Stop::returned, "DeleteLocalRef accepts a valid reference");
    check(f.call(slot(23), {java.env(), 0xe0001000}).stop == atlan::Stop::service_error, "DeleteLocalRef rejects a forged reference");
    check(f.call(slot(22), {java.env(), obj}).stop == atlan::Stop::service_error, "DeleteGlobalRef rejects a non-global");
    check(f.call(slot(22), {java.env(), global}).stop == atlan::Stop::returned && java.live_global_refs() == 0, "DeleteGlobalRef");
    check(f.call(slot(22), {java.env(), global}).stop == atlan::Stop::service_error, "double DeleteGlobalRef rejected");
    // Opaque host objects of any class, field store and registered methods.
    check(f.call(slot(33), {java.env(), cls, data + 0x280, data + 0x2a0}).stop == atlan::Stop::returned, "Thing ctor id");
    const auto thing_ctor = static_cast<std::uint32_t>(f.runtime.cpu.registers[0]);
    check(f.call(slot(28), {java.env(), cls, thing_ctor}).stop == atlan::Stop::returned, "app-style class instance");
    const auto thing = static_cast<std::uint32_t>(f.runtime.cpu.registers[0]);
    put_text(f.runtime.memory, data + 0x300, "J");
    check(f.call(slot(94), {java.env(), cls, data + 0x1c0, data + 0x1e0}).stop == atlan::Stop::returned, "int field id");
    const auto ifield = static_cast<std::uint32_t>(f.runtime.cpu.registers[0]);
    check(f.call(slot(100), {java.env(), thing, ifield}).stop == atlan::Stop::returned && f.runtime.cpu.registers[0] == 0, "unset field reads zero");
    check(f.call(slot(109), {java.env(), thing, ifield, 77}).stop == atlan::Stop::returned, "SetIntField");
    check(f.call(slot(100), {java.env(), thing, ifield}).stop == atlan::Stop::returned && f.runtime.cpu.registers[0] == 77, "GetIntField reads the stored value");
    check(f.call(slot(100), {java.env(), obj, ifield}).stop == atlan::Stop::service_error, "field of another class is rejected");
    check(f.call(slot(100), {java.env(), 0xe0000abc, ifield}).stop == atlan::Stop::service_error, "forged object is rejected");
    check(f.call(slot(94), {java.env(), cls, data + 0x180, data + 0x300}).stop == atlan::Stop::returned, "long field id");
    const auto jfield = static_cast<std::uint32_t>(f.runtime.cpu.registers[0]);
    // SetLongField: jlong on the stack because r3 is skipped for doubleword alignment.
    check(f.call(slot(110), {java.env(), thing, jfield, 0xbad, 0x11112222, 0x33334444}).stop == atlan::Stop::returned, "SetLongField");
    check(f.call(slot(101), {java.env(), thing, jfield}).stop == atlan::Stop::returned && f.runtime.cpu.registers[0] == 0x11112222 &&
          f.runtime.cpu.registers[1] == 0x33334444, "GetLongField returns r0:r1");
    // Registered method "run(I)V" on Thing: unregistered -> named stop, registered -> runs.
    auto run_id = [&]() { check(f.call(slot(33), {java.env(), cls, data + 0x180, data + 0x1a0}).stop == atlan::Stop::returned, "run id"); return static_cast<std::uint32_t>(f.runtime.cpu.registers[0]); }();
    r = f.call(slot(61), {java.env(), thing, run_id, 5});
    check(r.stop == atlan::Stop::service_error && r.message.find("no host implementation for pkg/Thing.run(I)V") != std::string::npos, "unregistered method stops by name");
    std::uint64_t seen{};
    check(java.register_method("pkg/Thing", "run", "(I)V", [&](atlan::JavaBootstrap::MethodCall& c) -> std::optional<std::string> { seen = c.args.at(0) + 1000ULL * c.receiver; return std::nullopt; }), "register method");
    check(!java.register_method("pkg/Thing", "run", "(I)V", [](atlan::JavaBootstrap::MethodCall&) { return std::nullopt; }), "duplicate registration refused");
    r = f.call(slot(61), {java.env(), thing, run_id, 5});
    check(r.stop == atlan::Stop::returned && seen == 5 + 1000ULL * thing, "CallVoidMethod runs the host method with its arguments");
    put32(f.runtime.memory, data + 0x700, 9);
    check(f.call(slot(62), {java.env(), thing, run_id, data + 0x700}).stop == atlan::Stop::returned && seen == 9 + 1000ULL * thing, "CallVoidMethodV reads the va_list");
    put64(f.runtime.memory, data + 0x710, 11);
    check(f.call(slot(63), {java.env(), thing, run_id, data + 0x710}).stop == atlan::Stop::returned && seen == 11 + 1000ULL * thing, "CallVoidMethodA reads jvalues");
    check(f.call(slot(49), {java.env(), thing, run_id, 5}).stop == atlan::Stop::service_error, "return type mismatch stops");
    check(f.call(slot(61), {java.env(), obj, run_id, 5}).stop == atlan::Stop::service_error, "receiver of another class stops");
    const auto host_cls = java.class_handle("pkg/Thing");
    check(host_cls == cls && java.class_handle("") == 0, "host class handle matches the guest's");
    const auto host_obj = java.new_object(host_cls);
    check(host_obj != 0 && java.new_object(0x1234) == 0 &&
          f.call(slot(100), {java.env(), host_obj, ifield}).stop == atlan::Stop::returned, "host-minted object works in guest field access");
    check(f.call(slot(215), {java.env(), 0xc0001000, data + 0x200, 1}).stop == atlan::Stop::service_error, "RegisterNatives rejects an unknown class");
    bool overlap = false;
    try { atlan::JavaBootstrap again(f.runtime, f.dispatcher, {}); } catch (const std::invalid_argument&) { overlap = true; }
    check(overlap, "a second bootstrap over the same region is rejected");
}
}
int main() {
    try {
        stack_arguments();
        log_print_formats();
        log_print_alignment();
        log_vprint_and_write();
        log_failures();
        jni_bootstrap();
    } catch (const std::exception& error) {
        std::cerr << "abi test failed: " << error.what() << '\n';
        return 1;
    }
    std::cout << "abi checks passed: " << checks << '\n';
}
