// SPDX-License-Identifier: GPL-2.0-or-later
#include "atlan/libc_services.hpp"
#include "atlan/lifetime.hpp"
#include <array>
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
constexpr std::uint32_t code = 0x1000, stack = 0x80000, sentinel = 0xffff0000;
constexpr std::uint32_t atexit_aeabi = 0xf0000000, atexit_cxa = 0xf0000010, finalize = 0xf0000020,
                        unknown = 0xf0000030, memset_trap = 0xf0000050, clock_trap = 0xf0000060, strlen_trap = 0xf0000070, log_word = 0x4000, dtor_log = 0x2000, dtor_loop = 0x2040,
                        dtor_thumb = 0x2101, atexit_data = 0xf0000040;

struct Fixture {
    atlan::Runtime runtime;
    atlan::ImportDispatcher dispatcher;
    atlan::ObjectLifetime lifetime;
    Fixture() {
        runtime.memory.map(code, 0x2000, atlan::Permissions::read_write);
        runtime.memory.map(0x4000, 0x1000, atlan::Permissions::read_write);
        runtime.memory.map(stack - 0x1000, 0x1000, atlan::Permissions::read_write);
        // Destructor: log = (log << 8) | r0; bx lr.
        const std::array<std::uint32_t, 6> body{0xe59f1010, 0xe5912000, 0xe1a02402, 0xe1822000, 0xe5812000, 0xe12fff1e};
        for (std::size_t i = 0; i < body.size(); ++i) put32(runtime.memory, 0x2000 + 4 * static_cast<std::uint32_t>(i), body[i]);
        put32(runtime.memory, 0x2018, log_word);
        put32(runtime.memory, dtor_loop, 0xeafffffe);  // b .
        put32(runtime.memory, 0x2100, 0x00004770);     // Thumb: bx lr
        runtime.memory.protect(code, 0x2000, atlan::Permissions::read_execute);
        atlan::elf::Image image;
        image.trapped_imports = {{"__aeabi_atexit", atexit_aeabi, 2}, {"__cxa_atexit", atexit_cxa, 2},
                                 {"__cxa_finalize", finalize, 2}, {"__never_registered", unknown, 2},
                                 {"data_symbol", atexit_data, 1},
                                 {"memset", memset_trap, 2}, {"clock_gettime", clock_trap, 2}, {"strlen", strlen_trap, 2}};
        dispatcher.attach(image);
        lifetime.install(dispatcher);
        atlan::install_libc_services(dispatcher);
    }
    atlan::CallOutcome call(std::uint32_t target, std::initializer_list<std::uint32_t> args,
                            std::uint64_t budget = 10000) {
        const std::vector<std::uint32_t> values(args);
        return dispatcher.call(runtime, target, values, stack, sentinel, budget);
    }
};

void registration_and_order() {
    Fixture f;
    check(f.call(atexit_aeabi, {0x11, dtor_log, 0x3000}).stop == atlan::Stop::returned, "aeabi_atexit returns");
    check(f.runtime.cpu.registers[0] == 0, "aeabi_atexit reports success in r0");
    check(f.lifetime.entries().size() == 1 && f.lifetime.entries()[0].function == dtor_log &&
          f.lifetime.entries()[0].object == 0x11 && f.lifetime.entries()[0].dso == 0x3000,
          "aeabi argument order stores object, destructor and DSO");
    check(f.call(atexit_cxa, {dtor_log, 0x22, 0x3000}).stop == atlan::Stop::returned, "cxa_atexit returns");
    check(f.lifetime.entries()[1].object == 0x22 && f.lifetime.entries()[1].function == dtor_log,
          "cxa argument order differs from aeabi");
    check(f.call(atexit_cxa, {dtor_log, 0x33, 0x5000}).stop == atlan::Stop::returned, "third registration");
    check(get32(f.runtime.memory, log_word) == 0, "registration never runs a destructor");

    f.runtime.cpu.registers[4] = 0x1234;
    const auto done = f.call(finalize, {0x3000});
    check(done.stop == atlan::Stop::returned, "finalize returns");
    check(get32(f.runtime.memory, log_word) == 0x2211, "finalize runs one DSO newest first");
    check(f.lifetime.entries().size() == 1 && f.lifetime.entries()[0].object == 0x33, "other DSO remains registered");
    check(f.runtime.cpu.registers[4] == 0x1234, "callee-saved state survives guest callbacks");
    check(f.call(finalize, {0}).stop == atlan::Stop::returned, "finalize all");
    check(get32(f.runtime.memory, log_word) == 0x221133, "remaining entries run");
    check(f.lifetime.entries().empty(), "entries are consumed");
    check(f.call(finalize, {0}).stop == atlan::Stop::returned && get32(f.runtime.memory, log_word) == 0x221133,
          "entries never run twice");
}
void thumb_destructor() {
    Fixture f;
    check(f.call(atexit_cxa, {dtor_thumb, 1, 7}).stop == atlan::Stop::returned, "register Thumb destructor");
    check(f.lifetime.entries()[0].function == dtor_thumb, "Thumb bit is preserved");
    check(f.call(finalize, {7}).stop == atlan::Stop::returned && f.lifetime.entries().empty(), "Thumb destructor runs");
}
void explicit_failures() {
    Fixture f;
    auto r = f.call(unknown, {});
    check(r.stop == atlan::Stop::unresolved_import && r.import == "__never_registered", "unbound import stops explicitly");
    r = f.call(atexit_data, {});
    check(r.stop == atlan::Stop::cpu_event, "data imports are not dispatched as functions");
    r = f.call(atexit_cxa, {0, 1, 2});
    check(r.stop == atlan::Stop::service_error && r.import == "__cxa_atexit" && f.lifetime.entries().empty(),
          "null destructor is rejected without registration");
    check(f.call(atexit_cxa, {dtor_loop, 1, 9}).stop == atlan::Stop::returned, "register looping destructor");
    r = f.call(finalize, {9}, 500);
    check(r.stop == atlan::Stop::service_error && r.message.find("did not return") != std::string::npos,
          "destructor budget is bounded and reported");
    check(f.lifetime.entries().empty(), "failed destructor is not retried");
    check(!f.dispatcher.bind("__cxa_atexit", [](atlan::ImportCall&) { return std::nullopt; }), "duplicate binding refused");
}
void libc() {
    Fixture f;
    put32(f.runtime.memory, 0x4100, 0xaaaaaaaa);
    put32(f.runtime.memory, 0x4104, 0xaaaaaaaa);
    check(f.call(memset_trap, {0x4101, 0x1ff, 3}).stop == atlan::Stop::returned, "memset returns");
    check(f.runtime.cpu.registers[0] == 0x4101, "memset returns dest");
    check(get32(f.runtime.memory, 0x4100) == 0xffffffaa && get32(f.runtime.memory, 0x4104) == 0xaaaaaaaa,
          "memset writes exactly count bytes of the low byte");
    check(f.call(memset_trap, {0x4101, 0, 0}).stop == atlan::Stop::returned, "zero-length memset");
    check(f.call(memset_trap, {0x9000000, 0, 4}).stop == atlan::Stop::service_error, "memset to unmapped memory fails");
    check(f.call(memset_trap, {0xfffffffe, 0, 4}).stop == atlan::Stop::service_error, "memset wrap fails");
    put32(f.runtime.memory, 0x4300, 0x00636261);  // "abc"
    check(f.call(strlen_trap, {0x4300}).stop == atlan::Stop::returned && f.runtime.cpu.registers[0] == 3, "strlen");
    check(f.call(strlen_trap, {0x9000000}).stop == atlan::Stop::service_error, "strlen stops at unmapped memory");
    check(f.call(clock_trap, {1, 0x4200}).stop == atlan::Stop::returned && f.runtime.cpu.registers[0] == 0,
          "clock_gettime monotonic");
    check(get32(f.runtime.memory, 0x4204) < 1000000000, "tv_nsec in range");
    check(f.call(clock_trap, {0, 0x4200}).stop == atlan::Stop::returned && get32(f.runtime.memory, 0x4200) > 1700000000,
          "clock_gettime realtime");
    check(f.call(clock_trap, {2, 0x4200}).stop == atlan::Stop::service_error, "unsupported clock rejected");
    check(f.call(clock_trap, {1, 0x9000000}).stop == atlan::Stop::service_error, "clock_gettime faults on bad pointer");
}
}
int main() {
    try {
        registration_and_order();
        thumb_destructor();
        explicit_failures();
        libc();
    } catch (const std::exception& error) {
        std::cerr << "import test failed: " << error.what() << '\n';
        return 1;
    }
    std::cout << "import checks passed: " << checks << '\n';
}
