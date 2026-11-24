// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include "atlan/imports.hpp"
#include <chrono>
#include <deque>
#include <functional>
#include <memory>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

namespace atlan {
class MutexTable;

// Cooperative guest threading. Every guest thread has its own CPU context (registers,
// TLS pointer, stack) but all of them run on the one host thread that owns the Runtime,
// switched by the ImportDispatcher only
//   * when a thread blocks in an import (mutex, condition variable, semaphore, join,
//     sleep, sched_yield), and
//   * when a thread has retired `quantum` instructions while another thread is ready
//     (so a guest spin-wait on a flag cannot starve the thread that sets it).
// Because exactly one context executes at a time, guest LDREX/STREX atomics, the
// software-paged Memory and every host service stay free of data races without any
// host locking, and scheduling is deterministic for a given clock. A switch discards
// the exclusive monitor, as a real context switch would.
//
// A blocking service is a retry loop (see ImportCall::block): it checks its condition,
// calls block() when the condition is false and leaves registers untouched, and is run
// again when the thread is woken. Guest callbacks made by host services (pthread_once
// initializers, thread-specific-data destructors run at exit are NOT such callbacks) run
// nested on the calling thread: they are never preempted and must not block.
//
// Not thread safe: use from the host thread that owns the Runtime only.
class GuestThreads {
public:
    struct Config {
        // Guest address range for thread stacks and TLS pages (must not overlap other mappings).
        std::uint32_t region_base = 0x71000000;
        std::uint32_t region_size = 64U * 1024 * 1024;
        std::uint32_t default_stack = 1024 * 1024;   // Bionic's default
        std::uint32_t max_stack = 8U * 1024 * 1024;
        // Unmapped, 4-byte aligned addresses reserved for the host (never mapped by the guest).
        std::uint32_t exit_sentinel = 0xfffe0000;     // start routines return here
        std::uint32_t callback_return = 0xfffe0004;   // return target of callbacks while pumping
        std::uint64_t quantum = 20000;                // instructions before a ready thread may preempt
        unsigned max_threads = 64;                    // live threads, including the main thread
        // Guest word holding errno. When set it is swapped on every context switch, so
        // errno is per thread without the guest knowing.
        std::uint32_t errno_address = 0;
        // Clocks (nanoseconds). Defaults: steady clock, system clock, real sleeping.
        // Tests inject a virtual clock whose sleep advances time.
        std::function<std::uint64_t()> now_ns;
        std::function<std::uint64_t()> realtime_ns;
        std::function<void(std::uint64_t)> sleep_until_ns;
    };
    enum class Pick : std::uint8_t { ran, idle, deadlock };

    // Per-thread scratch state of the blocking services.
    struct WaitState {
        bool timed_active{};          // a timed wait is in progress; deadline below is valid
        std::uint64_t deadline{};
        bool yield_pending{};
        std::uint32_t cond{}, mutex{}, saved_count{};  // condition variable wait
        std::uint8_t cond_phase{};                      // 0 idle, 1 waiting for signal, 2 reacquiring
        bool signaled{}, timed_out{};
    };

    GuestThreads();
    explicit GuestThreads(Config config);
    GuestThreads(const GuestThreads&) = delete;
    GuestThreads& operator=(const GuestThreads&) = delete;
    ~GuestThreads();

    // pthread_create/join/detach/self/exit/equal, pthread_attr_*, pthread_setname_np,
    // pthread_setschedparam, gettid, sched_yield, usleep, nanosleep, sleep.
    // Also registers itself with the dispatcher (ImportDispatcher::set_threads).
    void install(ImportDispatcher& dispatcher);

    // ---- services' view ----------------------------------------------------------------
    [[nodiscard]] std::uint32_t current_id() const noexcept;   // 0 outside any guest thread
    [[nodiscard]] std::uint32_t id_of_handle(std::uint32_t pthread) const noexcept;
    [[nodiscard]] std::uint64_t now_ns() const { return config_.now_ns(); }
    [[nodiscard]] WaitState& wait() noexcept;                   // current thread's wait state
    [[nodiscard]] WaitState* wait_of(std::uint32_t id) noexcept;
    [[nodiscard]] std::size_t live_threads() const noexcept { return threads_.size(); }
    // Marks every thread blocked on `key` runnable; they re-run their service.
    void wake(std::uint32_t key) noexcept;
    // Marks one blocked thread runnable regardless of its key.
    void make_ready(std::uint32_t id) noexcept;
    // Absolute POSIX timespec (guest memory) on `clock` (0 realtime, 1 monotonic) to a
    // now_ns() deadline. Returns 0, or EINVAL (22) for a malformed time, or -1 on a fault.
    [[nodiscard]] int deadline_from_timespec(Memory& memory, std::uint32_t timespec, std::uint32_t clock,
                                             std::uint64_t& deadline) const;
    // Writes errno; false if no errno address is configured.
    [[nodiscard]] bool set_errno(Memory& memory, std::uint32_t code);
    // Called with a thread id when it exits; returns (destructor, value) pairs to run on it.
    using ExitHook = std::function<std::vector<std::pair<std::uint32_t, std::uint32_t>>(std::uint32_t)>;
    void set_exit_hook(ExitHook hook) { exit_hook_ = std::move(hook); }

    // ---- dispatcher's view -------------------------------------------------------------
    [[nodiscard]] std::uint32_t exit_sentinel() const noexcept { return config_.exit_sentinel; }
    [[nodiscard]] std::uint32_t callback_return() const noexcept { return config_.callback_return; }
    [[nodiscard]] std::uint64_t quantum() const noexcept { return config_.quantum; }
    [[nodiscard]] bool has_current() const noexcept { return current_ != nullptr; }
    [[nodiscard]] bool current_is_main() const noexcept;
    // Starts an outermost run. pump: the caller's thread is not part of the schedule.
    void begin_run(Runtime& runtime, bool pump);
    // Ends it. normalize: put the Runtime back in the caller's context; otherwise the
    // context that stopped stays loaded for diagnosis and that thread is never resumed.
    void end_run(Runtime& runtime, bool normalize);
    // Loads the next runnable thread into the Runtime. May sleep (via the clock) until a
    // timed waiter is due when allow_sleep; otherwise idle means "nothing runnable now".
    Pick schedule(Runtime& runtime, bool allow_sleep);
    // The current thread blocks (its context is saved) and another is scheduled.
    Pick block_current(Runtime& runtime, std::uint32_t key, std::uint64_t deadline_ns,
                       std::string_view service, bool allow_sleep);
    // Switches to another ready thread if there is one.
    void yield_current(Runtime& runtime);
    // The current worker's start routine or a destructor returned to exit_sentinel.
    void thread_returned(Runtime& runtime);
    [[nodiscard]] std::string deadlock_message() const;
    // Earliest deadline of a timed waiter after an idle pump, in now_ns() time (0: none).
    [[nodiscard]] std::uint64_t next_wake_ns() const noexcept { return next_wake_; }

private:
    struct Thread;
    Thread* find_handle(std::uint32_t handle) const;
    Thread* make_thread(Runtime& runtime, std::uint32_t start, std::uint32_t argument,
                        std::uint32_t stack_size, bool detached, std::uint32_t& error);
    std::uint32_t ensure_main_handle(Runtime& runtime);
    std::uint32_t allocate(std::uint32_t bytes);
    void release(std::uint32_t base, std::uint32_t bytes);
    void reap(Runtime& runtime, Thread& thread);
    void park(Runtime& runtime);
    void load(Runtime& runtime, Thread& thread);
    void refresh();
    void push_ready(Thread& thread);
    void finish_current(Runtime& runtime);
    void exit_current(ImportCall& call, std::uint32_t value);
    void capture_errno(Runtime& runtime, Thread& thread);
    void apply_errno(Runtime& runtime, const Thread& thread);

    Config config_;
    std::vector<std::unique_ptr<Thread>> threads_;
    std::unordered_map<std::uint32_t, Thread*> by_handle_;
    std::deque<Thread*> ready_;
    Thread* current_{};
    Thread* main_{};
    bool pumping_{};
    bool main_active_{};
    CpuState host_cpu_{};
    std::uint32_t next_id_{1};
    std::uint32_t bump_{};
    std::vector<std::pair<std::uint32_t, std::uint32_t>> free_;  // (base, bytes)
    ExitHook exit_hook_;
    std::uint64_t next_wake_{};
};

// pthread mutexes. State is tracked by guest address in the host. A mutexattr word in
// guest memory holds its type (0 normal, 1 recursive, 2 error-checking, as in Bionic).
// With a GuestThreads scheduler a contended lock blocks the calling thread; without
// one, locking a held normal mutex can never succeed and is reported as a deadlock.
// Normal mutexes may be unlocked by any thread, as in Bionic.
class MutexTable {
public:
    explicit MutexTable(GuestThreads* threads = nullptr) : threads_(threads) {}
    void install(ImportDispatcher& dispatcher);
    [[nodiscard]] bool held(std::uint32_t address) const {
        const auto found = mutexes_.find(address);
        return found != mutexes_.end() && found->second.count != 0;
    }
    // For condition variables: give up / retake a mutex held by the current thread.
    // release returns the recursion count, or 0 if the current thread does not hold it.
    [[nodiscard]] std::uint32_t release(std::uint32_t address);
    [[nodiscard]] bool try_acquire(std::uint32_t address, std::uint32_t count);
private:
    struct State { std::uint32_t type{}; std::uint32_t count{}; std::uint32_t owner{}; };
    [[nodiscard]] std::uint32_t self() const noexcept { return threads_ ? threads_->current_id() : 0; }
    GuestThreads* threads_;
    std::unordered_map<std::uint32_t, State> mutexes_;  // absent: statically initialized, normal
};

// pthread condition variables, built on MutexTable and GuestThreads. The condattr word
// holds the clock id (0 realtime, 1 monotonic). Spurious wakeups never happen.
class CondTable {
public:
    CondTable(GuestThreads& threads, MutexTable& mutexes) : threads_(threads), mutexes_(mutexes) {}
    void install(ImportDispatcher& dispatcher);
private:
    struct State { std::uint32_t clock{}; std::deque<std::uint32_t> waiters; };
    std::optional<std::string> wait(ImportCall& call, bool timed, std::optional<std::uint32_t> clock_override);
    GuestThreads& threads_;
    MutexTable& mutexes_;
    std::unordered_map<std::uint32_t, State> conds_;
};

// POSIX semaphores, tracked by guest address in the host. Without a scheduler, waiting
// on a zero count can never be satisfied by another thread and stops with a deadlock
// diagnostic; with one, the thread blocks until sem_post.
class SemaphoreTable {
public:
    explicit SemaphoreTable(GuestThreads* threads = nullptr) : threads_(threads) {}
    void install(ImportDispatcher& dispatcher);
    [[nodiscard]] bool exists(std::uint32_t address) const { return counts_.contains(address); }
    [[nodiscard]] std::uint32_t value(std::uint32_t address) const {
        const auto found = counts_.find(address);
        return found == counts_.end() ? 0 : found->second;
    }
private:
    GuestThreads* threads_;
    std::unordered_map<std::uint32_t, std::uint32_t> counts_;
};

// Thread-specific data. Values are per guest thread (one value without a scheduler).
// With a scheduler, destructors of non-null values run when a thread exits (start
// routine return or pthread_exit), up to four rounds as POSIX requires.
class ThreadKeys {
public:
    static constexpr std::uint32_t max_keys = 128;
    explicit ThreadKeys(GuestThreads* threads = nullptr);
    void install(ImportDispatcher& dispatcher);
private:
    struct Key { bool live{}; std::uint32_t destructor{}; };
    [[nodiscard]] std::uint32_t self() const noexcept { return threads_ ? threads_->current_id() : 0; }
    GuestThreads* threads_;
    std::vector<Key> keys_;
    std::unordered_map<std::uint32_t, std::vector<std::uint32_t>> values_;  // thread id -> value per key
};
}
