// SPDX-License-Identifier: GPL-2.0-or-later
#include "atlan/files.hpp"
#include "atlan/heap.hpp"
#include "atlan/libc_services.hpp"
#include "atlan/threads.hpp"
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
                        dtor_thumb = 0x2101, atexit_data = 0xf0000040,
                        malloc_trap = 0xf0000080, free_trap = 0xf0000090, calloc_trap = 0xf00000a0,
                        realloc_trap = 0xf00000b0, strcpy_trap = 0xf00000c0, lock_trap = 0xf00000d0, unlock_trap = 0xf00000e0,
                        attr_init_trap = 0xf00000f0, attr_type_trap = 0xf0000100, mutex_init_trap = 0xf0000110,
                        memcpy_trap = 0xf0000120, once_trap = 0xf0000130, key_create_trap = 0xf0000140,
                        setspecific_trap = 0xf0000150, getspecific_trap = 0xf0000160, isalpha_trap = 0xf0000170,
                        ispunct_trap = 0xf0000180, wcslen_trap = 0xf0000190, wmemmove_trap = 0xf00001a0, fcntl_trap = 0xf00001b0, fstat_trap = 0xf00001c0, sysconf_trap = 0xf00001d0;

struct Fixture {
    atlan::Runtime runtime;
    atlan::ImportDispatcher dispatcher;
    atlan::ObjectLifetime lifetime;
    atlan::MutexTable mutexes;
    atlan::ThreadKeys keys;
    atlan::VirtualFiles files{0x4c00};
    atlan::GuestHeap heap{0x20000000, 4U * 1024 * 1024};
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
                                 {"memset", memset_trap, 2}, {"clock_gettime", clock_trap, 2}, {"strlen", strlen_trap, 2},
                                 {"malloc", malloc_trap, 2}, {"free", free_trap, 2}, {"calloc", calloc_trap, 2},
                                 {"realloc", realloc_trap, 2}, {"strcpy", strcpy_trap, 2},
                                 {"pthread_mutex_lock", lock_trap, 2}, {"pthread_mutex_unlock", unlock_trap, 2},
                                 {"pthread_mutexattr_init", attr_init_trap, 2}, {"pthread_mutexattr_settype", attr_type_trap, 2},
                                 {"pthread_mutex_init", mutex_init_trap, 2},
                                 {"memcpy", memcpy_trap, 2}, {"pthread_once", once_trap, 2}, {"pthread_key_create", key_create_trap, 2},
                                 {"pthread_setspecific", setspecific_trap, 2}, {"pthread_getspecific", getspecific_trap, 2},
                                 {"isalpha", isalpha_trap, 2}, {"ispunct", ispunct_trap, 2}, {"wcslen", wcslen_trap, 2},
                                 {"wmemmove", wmemmove_trap, 2}, {"fcntl", fcntl_trap, 2}, {"fstat", fstat_trap, 2}, {"sysconf", sysconf_trap, 2}};
        dispatcher.attach(image);
        lifetime.install(dispatcher);
        atlan::install_libc_services(dispatcher);
        atlan::install_heap_services(dispatcher, heap);
        mutexes.install(dispatcher);
        keys.install(dispatcher);
        files.install(dispatcher);
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
void heap_and_strings() {
    Fixture f;
    check(f.call(malloc_trap, {10}).stop == atlan::Stop::returned, "malloc returns");
    const auto a = static_cast<std::uint32_t>(f.runtime.cpu.registers[0]);
    check(a == 0x20000000 && a % 16 == 0, "first block is aligned at the heap base");
    f.call(malloc_trap, {0});
    const auto b = static_cast<std::uint32_t>(f.runtime.cpu.registers[0]);
    check(b == a + 16, "malloc(0) returns a distinct block");
    put32(f.runtime.memory, a, 0x11223344);
    check(f.call(realloc_trap, {a, 100}).stop == atlan::Stop::returned, "realloc grows");
    const auto c = static_cast<std::uint32_t>(f.runtime.cpu.registers[0]);
    check(c != a && get32(f.runtime.memory, c) == 0x11223344, "realloc preserves contents when moving");
    check(f.heap.block_size(a) == 0, "moved realloc frees the old block");
    f.call(realloc_trap, {c, 8});
    check(f.runtime.cpu.registers[0] == c, "shrinking realloc keeps the block");
    check(f.call(free_trap, {c}).stop == atlan::Stop::returned, "free");
    check(f.call(free_trap, {c}).stop == atlan::Stop::service_error, "double free is reported");
    check(f.call(free_trap, {0x20000001}).stop == atlan::Stop::service_error, "interior pointer free is reported");
    check(f.call(free_trap, {0}).stop == atlan::Stop::returned, "free(NULL) is a no-op");
    f.call(calloc_trap, {4, 8});
    const auto d = static_cast<std::uint32_t>(f.runtime.cpu.registers[0]);
    check(d == c, "freed block is reused first-fit");
    f.call(malloc_trap, {16}); const auto e = static_cast<std::uint32_t>(f.runtime.cpu.registers[0]);
    put32(f.runtime.memory, e, 0xffffffff); (void)f.call(free_trap, {e});
    f.call(calloc_trap, {4, 4});
    check(f.runtime.cpu.registers[0] == e && get32(f.runtime.memory, e) == 0, "calloc clears reused memory");
    f.call(calloc_trap, {0x10000, 0x10000});
    check(f.runtime.cpu.registers[0] == 0, "calloc overflow returns NULL");
    f.call(malloc_trap, {0x40000000});
    check(f.runtime.cpu.registers[0] == 0, "oversized malloc returns NULL");
    f.call(realloc_trap, {0, 32});
    check(f.runtime.cpu.registers[0] != 0, "realloc(NULL) allocates");
    f.call(realloc_trap, {d, 0});
    check(f.runtime.cpu.registers[0] == 0 && f.heap.block_size(d) == 0, "realloc(p,0) frees");
    check(f.call(realloc_trap, {0x20000001, 4}).stop == atlan::Stop::service_error, "realloc of a non-block is reported");
    // Coalescing: two adjacent frees satisfy a larger request in place.
    Fixture g;
    g.call(malloc_trap, {32}); const auto p = static_cast<std::uint32_t>(g.runtime.cpu.registers[0]);
    g.call(malloc_trap, {32}); const auto q = static_cast<std::uint32_t>(g.runtime.cpu.registers[0]);
    g.call(malloc_trap, {16});
    (void)g.call(free_trap, {q}); (void)g.call(free_trap, {p});
    g.call(malloc_trap, {64});
    check(g.runtime.cpu.registers[0] == p, "adjacent free blocks coalesce");
    check(g.call(lock_trap, {0x4400}).stop == atlan::Stop::returned && g.mutexes.held(0x4400), "mutex lock");
    check(g.call(lock_trap, {0x4400}).stop == atlan::Stop::service_error, "relocking a held mutex reports deadlock");
    check(g.call(unlock_trap, {0x4400}).stop == atlan::Stop::returned && !g.mutexes.held(0x4400), "mutex unlock");
    check(g.call(unlock_trap, {0x4400}).stop == atlan::Stop::returned && !g.mutexes.held(0x4400), "unlocking a free normal mutex succeeds as in Bionic");
    check(g.call(lock_trap, {0}).stop == atlan::Stop::service_error, "null mutex is reported");
    check(g.call(attr_init_trap, {0x4500}).stop == atlan::Stop::returned && get32(g.runtime.memory, 0x4500) == 0, "mutexattr_init");
    check(g.call(attr_type_trap, {0x4500, 1}).stop == atlan::Stop::returned && get32(g.runtime.memory, 0x4500) == 1, "settype recursive");
    check(g.call(mutex_init_trap, {0x4600, 0x4500}).stop == atlan::Stop::returned, "mutex_init with attribute");
    check(g.call(lock_trap, {0x4600}).stop == atlan::Stop::returned && g.call(lock_trap, {0x4600}).stop == atlan::Stop::returned,
          "recursive mutex relocks");
    (void)g.call(unlock_trap, {0x4600});
    check(g.mutexes.held(0x4600), "recursive mutex stays held until fully unlocked");
    (void)g.call(unlock_trap, {0x4600});
    check(!g.mutexes.held(0x4600), "recursive mutex releases");
    (void)g.call(attr_type_trap, {0x4500, 2}); (void)g.call(mutex_init_trap, {0x4700, 0x4500});
    (void)g.call(lock_trap, {0x4700});
    check(g.call(lock_trap, {0x4700}).stop == atlan::Stop::returned && g.runtime.cpu.registers[0] == 35, "error-checking relock returns EDEADLK");
    check(g.call(attr_type_trap, {0x4500, 9}).stop == atlan::Stop::returned && g.runtime.cpu.registers[0] == 22, "invalid type returns EINVAL");
    put32(g.runtime.memory, 0x4800, 0x04030201);
    check(g.call(memcpy_trap, {0x4801, 0x4800, 3}).stop == atlan::Stop::returned && get32(g.runtime.memory, 0x4800) == 0x03020101,
          "memcpy handles overlapping ranges like memmove");
    check(g.call(memcpy_trap, {0x4800, 0x9000000, 4}).stop == atlan::Stop::service_error, "memcpy from unmapped memory fails");
    put32(g.runtime.memory, 0x4900, 0);
    check(g.call(once_trap, {0x4900, 0x2018}).stop == atlan::Stop::service_error && get32(g.runtime.memory, 0x4900) == 1,
          "pthread_once whose routine faults is reported and not marked done");
    put32(g.runtime.memory, 0x4904, 2);
    check(g.call(once_trap, {0x4904, 0}).stop == atlan::Stop::returned, "completed pthread_once skips the routine");
    check(g.call(key_create_trap, {0x4a00, 0}).stop == atlan::Stop::returned && get32(g.runtime.memory, 0x4a00) == 0, "first key is 0");
    (void)g.call(setspecific_trap, {0, 0x1234});
    (void)g.call(getspecific_trap, {0});
    check(g.runtime.cpu.registers[0] == 0x1234, "thread key stores a value");
    (void)g.call(getspecific_trap, {7});
    check(g.runtime.cpu.registers[0] == 0, "invalid key reads NULL");
    check(g.call(setspecific_trap, {7, 1}).stop == atlan::Stop::returned && g.runtime.cpu.registers[0] == 22, "invalid key set returns EINVAL");
    (void)g.call(isalpha_trap, {'q'}); check(g.runtime.cpu.registers[0] == 1, "isalpha letter");
    (void)g.call(isalpha_trap, {'1'}); check(g.runtime.cpu.registers[0] == 0, "isalpha digit");
    (void)g.call(isalpha_trap, {0xffffffff}); check(g.runtime.cpu.registers[0] == 0, "EOF is not a character");
    (void)g.call(ispunct_trap, {'!'}); check(g.runtime.cpu.registers[0] == 1, "ispunct");
    put32(g.runtime.memory, 0x4b00, 0x41); put32(g.runtime.memory, 0x4b04, 0x42); put32(g.runtime.memory, 0x4b08, 0);
    (void)g.call(wcslen_trap, {0x4b00}); check(g.runtime.cpu.registers[0] == 2, "wcslen counts 32-bit characters");
    (void)g.call(wmemmove_trap, {0x4b04, 0x4b00, 2});
    check(get32(g.runtime.memory, 0x4b04) == 0x41 && get32(g.runtime.memory, 0x4b08) == 0x42, "wmemmove counts wide characters");
    (void)g.call(fcntl_trap, {1, 3, 0}); check(g.runtime.cpu.registers[0] == 1, "stdout is write-only");
    (void)g.call(fcntl_trap, {0, 3, 8}); check(g.runtime.cpu.registers[0] == 0, "stdin is read-only");
    (void)g.call(fcntl_trap, {1, 4, 04000}); (void)g.call(fcntl_trap, {1, 3, 0});
    check(g.runtime.cpu.registers[0] == (1 | 04000), "F_SETFL stores O_NONBLOCK");
    (void)g.call(fcntl_trap, {1, 2, 1}); (void)g.call(fcntl_trap, {1, 1, 0});
    check(g.runtime.cpu.registers[0] == 1, "F_SETFD stores close-on-exec");
    (void)g.call(fcntl_trap, {77, 3, 0});
    check(g.runtime.cpu.registers[0] == 0xffffffffU && get32(g.runtime.memory, 0x4c00) == 9, "unknown descriptor fails with EBADF");
    check(g.call(fcntl_trap, {1, 99, 0}).stop == atlan::Stop::service_error, "unsupported command stops explicitly");
    check(g.call(fstat_trap, {1, 0x4d00}).stop == atlan::Stop::returned && g.runtime.cpu.registers[0] == 0 &&
          get32(g.runtime.memory, 0x4d10) == 0020620, "fstat reports a character device");
    (void)g.call(fstat_trap, {55, 0x4d00}); check(g.runtime.cpu.registers[0] == 0xffffffffU, "fstat of an unknown descriptor fails");
    (void)g.call(sysconf_trap, {39}); check(g.runtime.cpu.registers[0] == 4096, "sysconf page size");
    check(g.call(sysconf_trap, {84}).stop == atlan::Stop::service_error, "unmodelled sysconf name stops explicitly");
    // strcpy
    put32(g.runtime.memory, 0x4300, 0x00636261);
    check(g.call(strcpy_trap, {p, 0x4300}).stop == atlan::Stop::returned && g.runtime.cpu.registers[0] == p &&
          get32(g.runtime.memory, p) == 0x00636261, "strcpy copies the terminator");
    check(g.call(strcpy_trap, {0x9000000, 0x4300}).stop == atlan::Stop::service_error, "strcpy to unmapped memory fails");
}
}
int main() {
    try {
        registration_and_order();
        thumb_destructor();
        explicit_failures();
        libc();
        heap_and_strings();
    } catch (const std::exception& error) {
        std::cerr << "import test failed: " << error.what() << '\n';
        return 1;
    }
    std::cout << "import checks passed: " << checks << '\n';
}
