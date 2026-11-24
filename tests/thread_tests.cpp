// SPDX-License-Identifier: GPL-2.0-or-later
#include "atlan/threads.hpp"
#include <array>
#include <cstring>
#include <iostream>
#include <stdexcept>

namespace {
#include "thread_guest.inc"
unsigned checks{};
void check(bool result, const char* message) {
    ++checks;
    if (!result) throw std::runtime_error(message);
}
constexpr std::uint32_t code = 0x1000, data = 0x4000, errno_word = 0x4f00, stack = 0x80000, sentinel = 0xffff0000;
struct Trap { const char* name; std::uint32_t address; };
const Trap traps[] = {{"pthread_create", 0xf0000000}, {"pthread_join", 0xf0000010}, {"pthread_exit", 0xf0000020},
                      {"sem_init", 0xf0000030}, {"sem_wait", 0xf0000040}, {"sem_post", 0xf0000050},
                      {"pthread_mutex_init", 0xf0000060}, {"pthread_mutex_lock", 0xf0000070},
                      {"pthread_mutex_unlock", 0xf0000080}, {"pthread_cond_init", 0xf0000090},
                      {"pthread_cond_wait", 0xf00000a0}, {"pthread_cond_signal", 0xf00000b0},
                      {"pthread_cond_timedwait", 0xf00000c0}, {"pthread_key_create", 0xf00000d0},
                      {"pthread_setspecific", 0xf00000e0}, {"pthread_getspecific", 0xf00000f0},
                      {"usleep", 0xf0000100}, {"sched_yield", 0xf0000110}, {"pthread_self", 0xf0000120},
                      {"sem_timedwait", 0xf0000130}, {"pthread_cond_broadcast", 0xf0000140}};
std::uint32_t get32(atlan::Memory& memory, std::uint32_t at) {
    std::array<std::byte, 4> bytes{};
    check(memory.read(at, bytes), "read guest word");
    std::uint32_t value{};
    for (unsigned i = 0; i < 4; ++i) value |= std::to_integer<std::uint32_t>(bytes[i]) << (8 * i);
    return value;
}
void put32(atlan::Memory& memory, std::uint32_t at, std::uint32_t value) {
    std::array<std::byte, 4> bytes{};
    for (unsigned i = 0; i < 4; ++i) bytes[i] = std::byte((value >> (8 * i)) & 255);
    check(memory.write(at, bytes), "write guest word");
}
// Virtual time: sleeping advances the clock instead of blocking the host.
struct Clock {
    std::uint64_t now = 1'000'000'000ULL, slept = 0;
    static constexpr std::uint64_t real_offset = 1'700'000'000'000'000'000ULL;
};
struct Fixture {
    Clock clock;
    atlan::Runtime runtime;
    atlan::ImportDispatcher dispatcher;
    atlan::GuestThreads threads;
    atlan::MutexTable mutexes{&threads};
    atlan::CondTable conds{threads, mutexes};
    atlan::SemaphoreTable semaphores{&threads};
    atlan::ThreadKeys keys{&threads};
    static atlan::GuestThreads::Config config(Clock& clock, std::uint64_t quantum) {
        atlan::GuestThreads::Config c;
        c.quantum = quantum;
        c.errno_address = errno_word;
        c.now_ns = [&clock] { return clock.now; };
        c.realtime_ns = [&clock] { return clock.now + Clock::real_offset; };
        c.sleep_until_ns = [&clock](std::uint64_t until) { if (until > clock.now) { clock.slept += until - clock.now; clock.now = until; } };
        return c;
    }
    explicit Fixture(std::uint64_t quantum = 1000, unsigned max_threads = 64)
        : threads([&] { auto c = config(clock, quantum); c.max_threads = max_threads; return c; }()) {
        runtime.memory.map(code, 0x2000, atlan::Permissions::read_write);
        runtime.memory.map(data, 0x1000, atlan::Permissions::read_write);
        runtime.memory.map(stack - 0x4000, 0x4000, atlan::Permissions::read_write);
        for (std::size_t i = 0; i < sizeof guest_words / 4; ++i) put32(runtime.memory, code + 4 * static_cast<std::uint32_t>(i), guest_words[i]);
        runtime.memory.protect(code, 0x2000, atlan::Permissions::read_execute);
        atlan::elf::Image image;
        for (const auto& t : traps) image.trapped_imports.push_back({t.name, t.address, 2});
        dispatcher.attach(image);
        threads.install(dispatcher);
        mutexes.install(dispatcher);
        conds.install(dispatcher);
        semaphores.install(dispatcher);
        keys.install(dispatcher);
    }
    atlan::CallOutcome call(std::uint32_t symbol, std::initializer_list<std::uint32_t> args, std::uint64_t budget = 200000) {
        const std::vector<std::uint32_t> values(args);
        return dispatcher.call(runtime, code + symbol, values, stack, sentinel, budget);
    }
    std::uint32_t r0() { return static_cast<std::uint32_t>(runtime.cpu.registers[0]); }
    std::uint32_t word(std::uint32_t offset) { return get32(runtime.memory, data + offset); }
};

void blocking_semaphore() {
    Fixture f;
    const auto out = f.call(sym_t_sem_workers, {data});
    check(out.stop == atlan::Stop::returned, "main returns after the worker finished");
    check(f.r0() == 5 && f.word(0) == 5, "worker ran five iterations and main saw them");
    check(f.word(12) == 42, "pthread_join delivers the start routine's return value");
    check(f.threads.live_threads() == 1, "joined thread is reaped");
}
void preemption() {
    Fixture f(200);
    const auto out = f.call(sym_t_spin, {data});
    check(out.stop == atlan::Stop::returned && f.r0() == 1, "a spinning thread is preempted so the setter runs");
    Fixture starved(1ULL << 40);  // effectively no preemption: the spin never ends
    const auto stuck = starved.call(sym_t_spin, {data}, 5000);
    check(stuck.stop == atlan::Stop::budget_exhausted, "without preemption the spin consumes the budget");
}
void condition_variable() {
    Fixture f;
    const auto out = f.call(sym_t_cond, {data});
    check(out.stop == atlan::Stop::returned && f.r0() == 1, "condition wait sees the flag set under the mutex");
    check(f.word(12) == 7, "worker result joined");
    check(!f.mutexes.held(data + 32), "mutex released at the end");
}
void thread_specific_data() {
    Fixture f;
    const auto out = f.call(sym_t_tls, {data});
    check(out.stop == atlan::Stop::returned, "tls program returns");
    check(f.r0() == 99, "main's value survives the worker's pthread_setspecific");
    check(f.word(48) == 0, "a new thread starts with no value for the key");
    check(f.word(52) == 7, "the destructor ran at thread exit with the worker's value");
    check(f.word(12) == 3, "exit sequence keeps the start routine's return value");
}
void explicit_exit() {
    Fixture f;
    const auto out = f.call(sym_t_exit, {data});
    check(out.stop == atlan::Stop::returned && f.r0() == 5, "pthread_exit value reaches pthread_join");
}
void sleeping() {
    Fixture f;
    const auto t0 = f.clock.now;
    const auto out = f.call(sym_t_sleep, {0, 2500});
    check(out.stop == atlan::Stop::returned && f.r0() == 1, "usleep returns");
    check(f.clock.now - t0 == 2'500'000, "usleep waited exactly its duration on the injected clock");
}
void timed_waits() {
    Fixture f;
    const std::uint64_t abs = f.clock.now + Clock::real_offset + 3'000'000;  // 3 ms ahead on the realtime clock
    put32(f.runtime.memory, data + 64, static_cast<std::uint32_t>(abs / 1'000'000'000ULL));
    put32(f.runtime.memory, data + 68, static_cast<std::uint32_t>(abs % 1'000'000'000ULL));
    const auto t0 = f.clock.now;
    auto out = f.call(sym_t_timedwait, {data});
    check(out.stop == atlan::Stop::returned && f.r0() == 110, "unsignaled pthread_cond_timedwait returns ETIMEDOUT");
    check(f.clock.now - t0 == 3'000'000, "timeout fires at the requested deadline");
    check(!f.mutexes.held(data + 32), "mutex was reacquired and then released by the guest");
    const std::uint64_t abs2 = f.clock.now + Clock::real_offset + 1'000'000;
    put32(f.runtime.memory, data + 64, static_cast<std::uint32_t>(abs2 / 1'000'000'000ULL));
    put32(f.runtime.memory, data + 68, static_cast<std::uint32_t>(abs2 % 1'000'000'000ULL));
    out = f.call(sym_t_semtimed, {data});
    check(out.stop == atlan::Stop::returned && f.r0() == 0xffffffffU, "sem_timedwait times out with -1");
    check(get32(f.runtime.memory, errno_word) == 110, "sem_timedwait sets errno to ETIMEDOUT");
}
void deadlock_is_reported() {
    Fixture f;
    const auto out = f.call(sym_t_deadlock, {data});
    check(out.stop == atlan::Stop::deadlock, "a lone thread waiting on an unposted semaphore deadlocks");
    check(out.message.find("sem_wait") != std::string::npos && out.message.find("thread 1") != std::string::npos,
          "the diagnostic names the blocked thread and import");
}
void pump_runs_background_threads() {
    Fixture f(1ULL << 40);
    auto out = f.call(sym_t_start_only, {data});
    check(out.stop == atlan::Stop::returned, "main returns without waiting");
    check(f.word(0) == 0, "the worker has not run: main never blocked or used up a quantum");
    const auto saved_pc = f.runtime.pc();
    out = f.dispatcher.pump(f.runtime, 12);
    check(out.stop == atlan::Stop::budget_exhausted, "a small pump budget stops mid-thread");
    check(f.word(0) > 0 && f.word(0) < 10 && f.runtime.pc() == saved_pc, "partial progress, caller context restored");
    out = f.dispatcher.pump(f.runtime, 100000);
    check(out.stop == atlan::Stop::returned, "pump ends idle");
    check(f.word(0) == 10, "pump ran the worker to completion");
    check(f.runtime.pc() == saved_pc, "pump leaves the caller's CPU context alone");
    out = f.call(sym_t_join_only, {data});
    check(out.stop == atlan::Stop::returned && f.r0() == 11, "a later call can join the finished thread");
    check(f.threads.live_threads() == 1, "reaped after join");
}
void yield_round_robin() {
    Fixture f;
    const auto out = f.call(sym_t_log, {data});
    check(out.stop == atlan::Stop::returned && f.r0() == 6, "both workers logged three letters");
    char log[7]{};
    for (unsigned i = 0; i < 6; ++i) {
        std::array<std::byte, 1> b{};
        check(f.runtime.memory.read(data + 0x100 + i, b), "read log");
        log[i] = static_cast<char>(std::to_integer<unsigned>(b[0]));
    }
    check(std::strcmp(log, "ABABAB") == 0, "sched_yield alternates ready threads in creation order");
}
void thread_limits() {
    Fixture f(1000, 2);  // main plus one
    const auto out = f.call(sym_t_log, {data});
    // The second pthread_create fails with EAGAIN; its handle word stays 0 and the join reports ESRCH.
    check(out.stop == atlan::Stop::returned, "creation failure is an error code, not a stop");
    check(f.word(60) == 0, "no handle is stored for a thread that was not created");
}
}
int main() {
    try {
        blocking_semaphore();
        preemption();
        condition_variable();
        thread_specific_data();
        explicit_exit();
        sleeping();
        timed_waits();
        deadlock_is_reported();
        pump_runs_background_threads();
        yield_round_robin();
        thread_limits();
        std::cout << "thread tests passed checks=" << checks << '\n';
    } catch (const std::exception& error) {
        std::cerr << "thread test failure: " << error.what() << '\n';
        return 1;
    }
}
