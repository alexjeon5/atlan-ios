// SPDX-License-Identifier: GPL-2.0-or-later
#include "atlan/threads.hpp"
#include <algorithm>
#include <array>
#include <sstream>
#include <stdexcept>
#include <thread>

namespace atlan {
namespace {
constexpr std::uint32_t eperm = 1, esrch = 3, eagain = 11, ebusy = 16, einval = 22, edeadlk = 35, etimedout = 110;
constexpr std::uint32_t page = Memory::page_size;
bool write_word(Memory& memory, std::uint32_t at, std::uint32_t value) {
    std::array<std::byte, 4> bytes{};
    for (unsigned i = 0; i < 4; ++i) bytes[i] = std::byte((value >> (8 * i)) & 255);
    return memory.write(at, bytes);
}
bool read_word(Memory& memory, std::uint32_t at, std::uint32_t& value) {
    std::array<std::byte, 4> bytes{};
    if (!memory.read(at, bytes)) return false;
    value = 0;
    for (unsigned i = 0; i < 4; ++i) value |= std::to_integer<std::uint32_t>(bytes[i]) << (8 * i);
    return true;
}
std::uint64_t round_up(std::uint64_t value) { return (value + page - 1) / page * page; }
std::string hex(std::uint32_t value) {
    std::ostringstream out;
    out << "0x" << std::hex << value;
    return out.str();
}
// Relative POSIX timespec to nanoseconds. 0 ok, einval for a malformed time, -1 for a fault.
int read_relative(Memory& memory, std::uint32_t at, std::uint64_t& ns) {
    std::uint32_t sec = 0, nsec = 0;
    if (!read_word(memory, at, sec) || !read_word(memory, at + 4, nsec)) return -1;
    if (nsec >= 1000000000U || static_cast<std::int32_t>(sec) < 0) return static_cast<int>(einval);
    ns = std::uint64_t{sec} * 1000000000ULL + nsec;
    return 0;
}
}

struct GuestThreads::Thread {
    enum class State : std::uint8_t { parked, ready, running, blocked, finished, faulted };
    enum class Phase : std::uint8_t { running, destructors };
    std::uint32_t id{};
    State state{State::ready};
    Phase phase{Phase::running};
    bool is_main{};
    bool detached{};
    CpuState cpu{};
    std::uint32_t handle{};          // guest pthread_t: address of the thread's TLS page
    std::uint32_t base{}, bytes{};   // allocation inside the region (0: none)
    std::uint32_t retval{};
    std::uint32_t block_key{};
    std::uint64_t deadline{};
    std::string wait_name;
    std::uint32_t errno_value{};
    unsigned rounds{};
    std::vector<std::pair<std::uint32_t, std::uint32_t>> pending;  // destructors still to run
    WaitState wait;
};

GuestThreads::GuestThreads() : GuestThreads(Config{}) {}
GuestThreads::GuestThreads(Config config) : config_(std::move(config)) {
    if (!config_.now_ns)
        config_.now_ns = [] {
            return static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(
                std::chrono::steady_clock::now().time_since_epoch()).count());
        };
    if (!config_.realtime_ns)
        config_.realtime_ns = [] {
            return static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(
                std::chrono::system_clock::now().time_since_epoch()).count());
        };
    if (!config_.sleep_until_ns)
        config_.sleep_until_ns = [this](std::uint64_t until) {
            const auto now = config_.now_ns();
            if (until > now) std::this_thread::sleep_for(std::chrono::nanoseconds(until - now));
        };
    if (config_.region_base % page != 0 || config_.region_size % page != 0 || config_.exit_sentinel % 4 != 0 ||
        config_.callback_return % 4 != 0 || config_.quantum == 0 || config_.max_threads == 0)
        throw std::invalid_argument("invalid guest thread configuration");
    bump_ = config_.region_base;
    auto main = std::make_unique<Thread>();
    main->id = next_id_++;
    main->is_main = true;
    main->state = Thread::State::parked;
    main_ = main.get();
    threads_.push_back(std::move(main));
}
GuestThreads::~GuestThreads() = default;

std::uint32_t GuestThreads::current_id() const noexcept { return current_ ? current_->id : 0; }
bool GuestThreads::current_is_main() const noexcept { return current_ != nullptr && current_->is_main; }
GuestThreads::WaitState& GuestThreads::wait() noexcept { return current_ ? current_->wait : main_->wait; }
GuestThreads::WaitState* GuestThreads::wait_of(std::uint32_t id) noexcept {
    for (auto& t : threads_) if (t->id == id) return &t->wait;
    return nullptr;
}
std::uint32_t GuestThreads::id_of_handle(std::uint32_t pthread) const noexcept {
    const auto* t = find_handle(pthread);
    return t ? t->id : 0;
}
GuestThreads::Thread* GuestThreads::find_handle(std::uint32_t handle) const {
    const auto found = by_handle_.find(handle);
    return found == by_handle_.end() ? nullptr : found->second;
}
int GuestThreads::deadline_from_timespec(Memory& memory, std::uint32_t timespec, std::uint32_t clock,
                                         std::uint64_t& deadline) const {
    std::uint64_t absolute = 0;
    if (const int rc = read_relative(memory, timespec, absolute); rc != 0) return rc;
    if (clock == 1) { deadline = absolute; return 0; }
    if (clock != 0) return static_cast<int>(einval);
    const auto real = config_.realtime_ns();
    deadline = config_.now_ns() + (absolute > real ? absolute - real : 0);
    return 0;
}
bool GuestThreads::set_errno(Memory& memory, std::uint32_t code) {
    return config_.errno_address != 0 && write_word(memory, config_.errno_address, code);
}
void GuestThreads::capture_errno(Runtime& runtime, Thread& thread) {
    if (config_.errno_address != 0) read_word(runtime.memory, config_.errno_address, thread.errno_value);
}
void GuestThreads::apply_errno(Runtime& runtime, const Thread& thread) {
    if (config_.errno_address != 0) write_word(runtime.memory, config_.errno_address, thread.errno_value);
}

std::uint32_t GuestThreads::allocate(std::uint32_t bytes) {
    for (auto it = free_.begin(); it != free_.end(); ++it)
        if (it->second == bytes) {
            const auto base = it->first;
            free_.erase(it);
            return base;
        }
    if (std::uint64_t{bump_} + bytes > std::uint64_t{config_.region_base} + config_.region_size) return 0;
    const auto base = bump_;
    bump_ += bytes;
    return base;
}
void GuestThreads::release(std::uint32_t base, std::uint32_t bytes) { free_.emplace_back(base, bytes); }

std::uint32_t GuestThreads::ensure_main_handle(Runtime& runtime) {
    if (main_->handle != 0) return main_->handle;
    const auto base = allocate(page);
    if (base == 0) throw std::runtime_error("guest thread region is exhausted");
    runtime.memory.map(base, page, Permissions::read_write);
    write_word(runtime.memory, base, base);
    write_word(runtime.memory, base + 4, main_->id);
    main_->handle = base;
    main_->base = base;
    main_->bytes = page;
    by_handle_[base] = main_;
    // The embedder normally provides the main thread's TLS; fill it only if it did not.
    if (runtime.cpu.a32_tpidruro == 0) runtime.cpu.a32_tpidruro = base;
    return base;
}

GuestThreads::Thread* GuestThreads::make_thread(Runtime& runtime, std::uint32_t start, std::uint32_t argument,
                                                std::uint32_t stack_size, bool detached, std::uint32_t& error) {
    error = 0;
    if (threads_.size() >= config_.max_threads) { error = eagain; return nullptr; }
    std::uint64_t stack = stack_size == 0 ? config_.default_stack : stack_size;
    stack = round_up(std::clamp<std::uint64_t>(stack, 16 * 1024, config_.max_stack));
    const auto bytes = static_cast<std::uint32_t>(page + stack + page);  // guard gap, stack, TLS page
    const auto base = allocate(bytes);
    if (base == 0) { error = eagain; return nullptr; }
    const auto tls = static_cast<std::uint32_t>(base + page + stack);
    try {
        runtime.memory.map(base + page, stack + page, Permissions::read_write);
    } catch (const std::exception&) {
        release(base, bytes);
        error = eagain;
        return nullptr;
    }
    auto thread = std::make_unique<Thread>();
    thread->id = next_id_++;
    thread->handle = tls;
    thread->base = base;
    thread->bytes = bytes;
    thread->detached = detached;
    write_word(runtime.memory, tls, tls);            // TLS slot 0: self pointer
    write_word(runtime.memory, tls + 4, thread->id); // TLS slot 1: thread id
    CpuState fresh = runtime.cpu;                    // inherit mode bits (FPSCR, CP15BEN), reset the rest
    fresh.registers.fill(0);
    fresh.nzcv = 0;
    fresh.vectors = {};
    fresh.a32_itstate = 0;
    fresh.a32_tpidruro = tls;
    fresh.registers[0] = argument;
    fresh.registers[13] = tls;
    fresh.registers[14] = config_.exit_sentinel;
    const CpuState saved = runtime.cpu;
    runtime.cpu = fresh;
    runtime.start(start);  // sets the PC key and ARM/Thumb state from the target's bit 0
    thread->cpu = runtime.cpu;
    runtime.cpu = saved;
    Thread* raw = thread.get();
    by_handle_[tls] = raw;
    threads_.push_back(std::move(thread));
    push_ready(*raw);
    return raw;
}

void GuestThreads::reap(Runtime& runtime, Thread& thread) {
    if (thread.is_main) return;
    runtime.memory.unmap(thread.base, thread.bytes);
    release(thread.base, thread.bytes);
    by_handle_.erase(thread.handle);
    std::erase_if(ready_, [&](Thread* t) { return t == &thread; });
    std::erase_if(threads_, [&](const std::unique_ptr<Thread>& t) { return t.get() == &thread; });
}

void GuestThreads::push_ready(Thread& thread) {
    if (thread.is_main && !main_active_) return;
    thread.state = Thread::State::ready;
    ready_.push_back(&thread);
}
void GuestThreads::wake(std::uint32_t key) noexcept {
    if (key == 0) return;
    for (auto& t : threads_)
        if (t->state == Thread::State::blocked && t->block_key == key) push_ready(*t);
}
void GuestThreads::make_ready(std::uint32_t id) noexcept {
    for (auto& t : threads_)
        if (t->id == id && t->state == Thread::State::blocked) push_ready(*t);
}
void GuestThreads::refresh() {
    bool timed = false;
    for (const auto& t : threads_) timed = timed || (t->state == Thread::State::blocked && t->deadline != 0);
    if (!timed) return;
    const auto now = config_.now_ns();
    for (auto& t : threads_)
        if (t->state == Thread::State::blocked && t->deadline != 0 && now >= t->deadline) push_ready(*t);
}

void GuestThreads::load(Runtime& runtime, Thread& thread) {
    thread.state = Thread::State::running;
    current_ = &thread;
    runtime.cpu = thread.cpu;
    runtime.cpu.clear_exclusive_monitor();
    apply_errno(runtime, thread);
}
void GuestThreads::park(Runtime& runtime) {
    current_->cpu = runtime.cpu;
    capture_errno(runtime, *current_);
    current_ = nullptr;
}

void GuestThreads::begin_run(Runtime& runtime, bool pump) {
    pumping_ = pump;
    next_wake_ = 0;
    if (!pump) {
        std::erase_if(ready_, [&](Thread* t) { return t == main_; });
        main_active_ = true;
        main_->state = Thread::State::running;
        main_->block_key = 0;
        main_->deadline = 0;
        main_->wait = {};
        current_ = main_;
        ensure_main_handle(runtime);
    } else {
        host_cpu_ = runtime.cpu;
        capture_errno(runtime, *main_);
        current_ = nullptr;
    }
}
void GuestThreads::end_run(Runtime& runtime, bool normalize) {
    if (!normalize) {
        // Diagnostic stop: the faulting context stays loaded and is never resumed.
        if (current_ != nullptr) { current_->state = Thread::State::faulted; current_ = nullptr; }
        main_active_ = false;
        return;
    }
    if (pumping_) {
        if (current_ != nullptr) { const auto t = current_; park(runtime); push_ready(*t); }
        runtime.cpu = host_cpu_;
        apply_errno(runtime, *main_);
    } else {
        if (current_ != nullptr && !current_->is_main) {
            const auto t = current_;
            park(runtime);
            push_ready(*t);
            runtime.cpu = main_->cpu;
            apply_errno(runtime, *main_);
        } else if (current_ != nullptr) {
            capture_errno(runtime, *main_);
        }
        current_ = nullptr;
        main_->state = Thread::State::parked;
        main_active_ = false;
    }
}

GuestThreads::Pick GuestThreads::schedule(Runtime& runtime, bool allow_sleep) {
    next_wake_ = 0;
    while (true) {
        refresh();
        while (!ready_.empty()) {
            Thread* next = ready_.front();
            ready_.pop_front();
            if (next->state != Thread::State::ready) continue;
            load(runtime, *next);
            return Pick::ran;
        }
        std::uint64_t earliest = 0;
        for (const auto& t : threads_)
            if (t->state == Thread::State::blocked && t->deadline != 0 && (!t->is_main || main_active_) &&
                (earliest == 0 || t->deadline < earliest))
                earliest = t->deadline;
        if (earliest == 0) return allow_sleep ? Pick::deadlock : Pick::idle;
        if (!allow_sleep) { next_wake_ = earliest; return Pick::idle; }
        config_.sleep_until_ns(earliest);
    }
}
GuestThreads::Pick GuestThreads::block_current(Runtime& runtime, std::uint32_t key, std::uint64_t deadline_ns,
                                               std::string_view service, bool allow_sleep) {
    Thread& t = *current_;
    t.state = Thread::State::blocked;
    t.block_key = key;
    t.deadline = deadline_ns;
    t.wait_name = std::string(service);
    park(runtime);
    return schedule(runtime, allow_sleep);
}
void GuestThreads::yield_current(Runtime& runtime) {
    refresh();
    if (ready_.empty()) return;
    Thread* self = current_;
    park(runtime);
    push_ready(*self);
    schedule(runtime, false);
}

void GuestThreads::finish_current(Runtime& runtime) {
    Thread& t = *current_;
    current_ = nullptr;
    t.state = Thread::State::finished;
    wake(t.handle);
    if (t.detached) reap(runtime, t);
}
void GuestThreads::thread_returned(Runtime& runtime) {
    Thread& t = *current_;
    if (t.phase == Thread::Phase::running) {
        t.retval = static_cast<std::uint32_t>(runtime.cpu.registers[0]);
        t.phase = Thread::Phase::destructors;
    }
    while (true) {
        if (t.pending.empty() && exit_hook_ && t.rounds < 4) {
            t.pending = exit_hook_(t.id);
            if (!t.pending.empty()) ++t.rounds;
        }
        if (t.pending.empty()) break;
        const auto [routine, value] = t.pending.front();
        t.pending.erase(t.pending.begin());
        if (routine == 0) continue;
        runtime.cpu.registers[0] = value;
        runtime.cpu.registers[14] = config_.exit_sentinel;
        runtime.cpu.registers[13] &= ~std::uint64_t{7};
        runtime.start(routine);
        return;
    }
    finish_current(runtime);
}
void GuestThreads::exit_current(ImportCall& call, std::uint32_t value) {
    call.runtime().cpu.registers[0] = value;
    call.runtime().start(config_.exit_sentinel);  // the dispatcher sees the sentinel and runs the exit sequence
    call.transfer();
}
std::string GuestThreads::deadlock_message() const {
    std::string text = "deadlock: every guest thread is blocked";
    for (const auto& t : threads_) {
        if (t->state != Thread::State::blocked || (t->is_main && !main_active_)) continue;
        text += "; thread " + std::to_string(t->id) + (t->is_main ? " (main)" : "") + " in " + t->wait_name;
        if (t->block_key != 0) text += " on " + hex(t->block_key);
    }
    return text;
}

// ---- GuestThreads services ------------------------------------------------------------
namespace {
// Blocks for `ns` from the first call; later calls (retries) finish once the time has passed.
void sleep_for_ns(GuestThreads& threads, ImportCall& call, std::uint64_t ns) {
    auto& w = threads.wait();
    if (!w.timed_active) {
        if (ns == 0) { call.set_result(0); return; }
        w.timed_active = true;
        w.deadline = threads.now_ns() + ns;
    }
    if (threads.now_ns() >= w.deadline) { w.timed_active = false; call.set_result(0); }
    else call.block(0, w.deadline);
}
}
void GuestThreads::install(ImportDispatcher& dispatcher) {
    dispatcher.set_threads(this);
    constexpr std::uint32_t attr_flags = 0, attr_stack_size = 8, attr_guard = 12, attr_policy = 16, attr_priority = 20;
    // int pthread_attr_init(pthread_attr_t*): Bionic layout, 24 bytes.
    dispatcher.bind("pthread_attr_init", [this](ImportCall& call) -> std::optional<std::string> {
        auto& m = call.runtime().memory;
        const std::uint32_t at = call.arg(0), words[6]{0, 0, config_.default_stack, page, 0, 0};
        for (unsigned i = 0; i < 6; ++i)
            if (!write_word(m, at + 4 * i, words[i])) return "pthread_attr_init guest memory fault";
        call.set_result(0);
        return std::nullopt;
    });
    dispatcher.bind("pthread_attr_destroy", [](ImportCall& call) -> std::optional<std::string> { call.set_result(0); return std::nullopt; });
    dispatcher.bind("pthread_attr_setdetachstate", [](ImportCall& call) -> std::optional<std::string> {
        std::uint32_t flags = 0;
        if (call.arg(1) > 1) { call.set_result(einval); return std::nullopt; }
        if (!read_word(call.runtime().memory, call.arg(0) + attr_flags, flags)) return "pthread_attr_setdetachstate guest memory fault";
        flags = (flags & ~1U) | call.arg(1);
        if (!write_word(call.runtime().memory, call.arg(0) + attr_flags, flags)) return "pthread_attr_setdetachstate guest memory fault";
        call.set_result(0);
        return std::nullopt;
    });
    dispatcher.bind("pthread_attr_getdetachstate", [](ImportCall& call) -> std::optional<std::string> {
        std::uint32_t flags = 0;
        if (!read_word(call.runtime().memory, call.arg(0) + attr_flags, flags) ||
            !write_word(call.runtime().memory, call.arg(1), flags & 1U)) return "pthread_attr_getdetachstate guest memory fault";
        call.set_result(0);
        return std::nullopt;
    });
    dispatcher.bind("pthread_attr_setstacksize", [](ImportCall& call) -> std::optional<std::string> {
        if (call.arg(1) < 16 * 1024) { call.set_result(einval); return std::nullopt; }
        if (!write_word(call.runtime().memory, call.arg(0) + attr_stack_size, call.arg(1))) return "pthread_attr_setstacksize guest memory fault";
        call.set_result(0);
        return std::nullopt;
    });
    dispatcher.bind("pthread_attr_getstacksize", [](ImportCall& call) -> std::optional<std::string> {
        std::uint32_t size = 0;
        if (!read_word(call.runtime().memory, call.arg(0) + attr_stack_size, size) ||
            !write_word(call.runtime().memory, call.arg(1), size)) return "pthread_attr_getstacksize guest memory fault";
        call.set_result(0);
        return std::nullopt;
    });
    dispatcher.bind("pthread_attr_setguardsize", [](ImportCall& call) -> std::optional<std::string> {
        if (!write_word(call.runtime().memory, call.arg(0) + attr_guard, call.arg(1))) return "pthread_attr_setguardsize guest memory fault";
        call.set_result(0);
        return std::nullopt;
    });
    dispatcher.bind("pthread_attr_getguardsize", [](ImportCall& call) -> std::optional<std::string> {
        std::uint32_t size = 0;
        if (!read_word(call.runtime().memory, call.arg(0) + attr_guard, size) ||
            !write_word(call.runtime().memory, call.arg(1), size)) return "pthread_attr_getguardsize guest memory fault";
        call.set_result(0);
        return std::nullopt;
    });
    dispatcher.bind("pthread_attr_setschedpolicy", [](ImportCall& call) -> std::optional<std::string> {
        if (!write_word(call.runtime().memory, call.arg(0) + attr_policy, call.arg(1))) return "pthread_attr_setschedpolicy guest memory fault";
        call.set_result(0);
        return std::nullopt;
    });
    dispatcher.bind("pthread_attr_getschedpolicy", [](ImportCall& call) -> std::optional<std::string> {
        std::uint32_t policy = 0;
        if (!read_word(call.runtime().memory, call.arg(0) + attr_policy, policy) ||
            !write_word(call.runtime().memory, call.arg(1), policy)) return "pthread_attr_getschedpolicy guest memory fault";
        call.set_result(0);
        return std::nullopt;
    });
    // int pthread_attr_setschedparam(attr*, const sched_param*): the only field is the priority.
    dispatcher.bind("pthread_attr_setschedparam", [](ImportCall& call) -> std::optional<std::string> {
        std::uint32_t priority = 0;
        if (!read_word(call.runtime().memory, call.arg(1), priority) ||
            !write_word(call.runtime().memory, call.arg(0) + attr_priority, priority)) return "pthread_attr_setschedparam guest memory fault";
        call.set_result(0);
        return std::nullopt;
    });
    dispatcher.bind("pthread_attr_getschedparam", [](ImportCall& call) -> std::optional<std::string> {
        std::uint32_t priority = 0;
        if (!read_word(call.runtime().memory, call.arg(0) + attr_priority, priority) ||
            !write_word(call.runtime().memory, call.arg(1), priority)) return "pthread_attr_getschedparam guest memory fault";
        call.set_result(0);
        return std::nullopt;
    });
    // Scheduling scope, names and priorities have no meaning for cooperative guest threads.
    dispatcher.bind("pthread_attr_setscope", [](ImportCall& call) -> std::optional<std::string> { call.set_result(0); return std::nullopt; });
    dispatcher.bind("pthread_setname_np", [](ImportCall& call) -> std::optional<std::string> { call.set_result(0); return std::nullopt; });
    dispatcher.bind("pthread_setschedparam", [](ImportCall& call) -> std::optional<std::string> { call.set_result(0); return std::nullopt; });

    // int pthread_create(pthread_t*, const pthread_attr_t*, void* (*start)(void*), void*)
    dispatcher.bind("pthread_create", [this](ImportCall& call) -> std::optional<std::string> {
        auto& runtime = call.runtime();
        std::uint32_t flags = 0, stack = 0;
        if (call.arg(1) != 0 && (!read_word(runtime.memory, call.arg(1) + attr_flags, flags) ||
                                 !read_word(runtime.memory, call.arg(1) + attr_stack_size, stack)))
            return "pthread_create could not read the thread attributes";
        if (call.arg(2) == 0) { call.set_result(einval); return std::nullopt; }
        std::uint32_t error = 0;
        ensure_main_handle(runtime);
        const Thread* thread = make_thread(runtime, call.arg(2), call.arg(3), stack, (flags & 1U) != 0, error);
        if (thread == nullptr) { call.set_result(error); return std::nullopt; }
        if (!write_word(runtime.memory, call.arg(0), thread->handle)) return "pthread_create could not store the thread handle";
        call.set_result(0);
        return std::nullopt;
    });
    // int pthread_join(pthread_t, void** result)
    dispatcher.bind("pthread_join", [this](ImportCall& call) -> std::optional<std::string> {
        Thread* target = find_handle(call.arg(0));
        if (target == nullptr) { call.set_result(esrch); return std::nullopt; }
        if (target == current_) { call.set_result(edeadlk); return std::nullopt; }
        if (target->detached) { call.set_result(einval); return std::nullopt; }
        if (target->state != Thread::State::finished) {
            if (target->state == Thread::State::faulted) return "pthread_join of a guest thread that stopped on an error";
            call.block(target->handle);
            return std::nullopt;
        }
        if (call.arg(1) != 0 && !write_word(call.runtime().memory, call.arg(1), target->retval))
            return "pthread_join guest memory fault";
        reap(call.runtime(), *target);
        call.set_result(0);
        return std::nullopt;
    });
    // int pthread_detach(pthread_t)
    dispatcher.bind("pthread_detach", [this](ImportCall& call) -> std::optional<std::string> {
        Thread* target = find_handle(call.arg(0));
        if (target == nullptr) { call.set_result(esrch); return std::nullopt; }
        if (target->detached) { call.set_result(einval); return std::nullopt; }
        target->detached = true;
        if (target->state == Thread::State::finished) reap(call.runtime(), *target);
        call.set_result(0);
        return std::nullopt;
    });
    // pthread_t pthread_self(void)
    dispatcher.bind("pthread_self", [this](ImportCall& call) -> std::optional<std::string> {
        call.set_result(current_ && !current_->is_main ? current_->handle : ensure_main_handle(call.runtime()));
        return std::nullopt;
    });
    dispatcher.bind("pthread_equal", [](ImportCall& call) -> std::optional<std::string> {
        call.set_result(call.arg(0) == call.arg(1) ? 1 : 0);
        return std::nullopt;
    });
    // pid_t gettid(void)
    dispatcher.bind("gettid", [this](ImportCall& call) -> std::optional<std::string> {
        call.set_result(current_id());
        return std::nullopt;
    });
    // void pthread_exit(void*)
    dispatcher.bind("pthread_exit", [this](ImportCall& call) -> std::optional<std::string> {
        if (current_ == nullptr || current_->is_main) return "pthread_exit on the main guest thread is not supported";
        if (call.dispatcher().depth() > 1) return "pthread_exit inside a host-initiated guest callback is not supported";
        exit_current(call, call.arg(0));
        return std::nullopt;
    });
    // int sched_yield(void): retried once after every other ready thread had a turn.
    dispatcher.bind("sched_yield", [this](ImportCall& call) -> std::optional<std::string> {
        auto& w = wait();
        if (!w.yield_pending) { w.yield_pending = true; call.block(0, 1); return std::nullopt; }  // deadline 1: already due
        w.yield_pending = false;
        call.set_result(0);
        return std::nullopt;
    });
    // int usleep(useconds_t), unsigned sleep(unsigned), int nanosleep(const timespec*, timespec*)
    dispatcher.bind("usleep", [this](ImportCall& call) -> std::optional<std::string> {
        sleep_for_ns(*this, call, std::uint64_t{call.arg(0)} * 1000ULL);
        return std::nullopt;
    });
    dispatcher.bind("sleep", [this](ImportCall& call) -> std::optional<std::string> {
        sleep_for_ns(*this, call, std::uint64_t{call.arg(0)} * 1000000000ULL);
        return std::nullopt;
    });
    dispatcher.bind("nanosleep", [this](ImportCall& call) -> std::optional<std::string> {
        std::uint64_t ns = 0;
        if (!wait().timed_active) {
            const int rc = read_relative(call.runtime().memory, call.arg(0), ns);
            if (rc < 0) return "nanosleep guest memory fault";
            if (rc != 0) {
                if (!set_errno(call.runtime().memory, einval)) return "nanosleep with an invalid time (errno is not configured)";
                call.set_result(0xffffffffU);
                return std::nullopt;
            }
        }
        sleep_for_ns(*this, call, ns);
        return std::nullopt;
    });
}

// ---- mutexes ----------------------------------------------------------------------------
std::uint32_t MutexTable::release(std::uint32_t address) {
    const auto found = mutexes_.find(address);
    if (found == mutexes_.end() || found->second.count == 0 || found->second.owner != self()) return 0;
    const auto count = found->second.count;
    found->second.count = 0;
    found->second.owner = 0;
    if (threads_) threads_->wake(address);
    return count;
}
bool MutexTable::try_acquire(std::uint32_t address, std::uint32_t count) {
    auto& state = mutexes_[address];
    if (state.count != 0) return false;
    state.count = count;
    state.owner = self();
    return true;
}
void MutexTable::install(ImportDispatcher& dispatcher) {
    // int pthread_once(pthread_once_t*, void (*init)(void)): 0 = not run, 1 = running, 2 = done.
    // The initializer runs as a nested host callback, which is never preempted, so state 1
    // can only be seen by the initializer itself.
    dispatcher.bind("pthread_once", [](ImportCall& call) -> std::optional<std::string> {
        const auto control = call.arg(0), routine = call.arg(1);
        auto& memory = call.runtime().memory;
        std::uint32_t state = 0;
        if (!read_word(memory, control, state)) return "pthread_once could not read the control word";
        if (state == 2) { call.set_result(0); return std::nullopt; }
        if (state == 1) return "pthread_once re-entered by its own init routine (deadlock)";
        if (routine == 0) return "pthread_once with a null init routine";
        if (!write_word(memory, control, 1)) return "pthread_once guest memory fault";
        const auto outcome = call.dispatcher().call_back(call.runtime(), routine, {});
        if (outcome.stop != Stop::returned)
            return "pthread_once init routine did not return" +
                   (outcome.import.empty() ? std::string() : " (import " + outcome.import + ")");
        if (!write_word(memory, control, 2)) return "pthread_once guest memory fault";
        call.set_result(0);
        return std::nullopt;
    });
    // int pthread_mutexattr_init(pthread_mutexattr_t*)
    dispatcher.bind("pthread_mutexattr_init", [](ImportCall& call) -> std::optional<std::string> {
        if (!write_word(call.runtime().memory, call.arg(0), 0)) return "pthread_mutexattr_init guest memory fault";
        call.set_result(0);
        return std::nullopt;
    });
    // int pthread_mutexattr_settype(pthread_mutexattr_t*, int type)
    dispatcher.bind("pthread_mutexattr_settype", [](ImportCall& call) -> std::optional<std::string> {
        if (call.arg(1) > 2) { call.set_result(einval); return std::nullopt; }
        if (!write_word(call.runtime().memory, call.arg(0), call.arg(1))) return "pthread_mutexattr_settype guest memory fault";
        call.set_result(0);
        return std::nullopt;
    });
    // int pthread_mutexattr_destroy(pthread_mutexattr_t*)
    dispatcher.bind("pthread_mutexattr_destroy", [](ImportCall& call) -> std::optional<std::string> {
        call.set_result(0);
        return std::nullopt;
    });
    // int pthread_mutex_init(pthread_mutex_t*, const pthread_mutexattr_t*)
    dispatcher.bind("pthread_mutex_init", [this](ImportCall& call) -> std::optional<std::string> {
        std::uint32_t type = 0;
        if (call.arg(1) != 0 && !read_word(call.runtime().memory, call.arg(1), type))
            return "pthread_mutex_init could not read the attribute";
        if (type > 2) { call.set_result(einval); return std::nullopt; }
        if (call.arg(0) == 0) return "pthread_mutex_init on a null mutex";
        mutexes_[call.arg(0)] = {type, 0, 0};
        call.set_result(0);
        return std::nullopt;
    });
    // int pthread_mutex_destroy(pthread_mutex_t*)
    dispatcher.bind("pthread_mutex_destroy", [this](ImportCall& call) -> std::optional<std::string> {
        const auto found = mutexes_.find(call.arg(0));
        if (found != mutexes_.end() && found->second.count != 0) { call.set_result(ebusy); return std::nullopt; }
        mutexes_.erase(call.arg(0));
        call.set_result(0);
        return std::nullopt;
    });
    // int pthread_mutex_lock(pthread_mutex_t*)
    dispatcher.bind("pthread_mutex_lock", [this](ImportCall& call) -> std::optional<std::string> {
        const auto address = call.arg(0);
        if (address == 0) return "pthread_mutex_lock on a null mutex";
        auto& state = mutexes_[address];
        if (state.count == 0) {
            state.count = 1;
            state.owner = self();
            call.set_result(0);
        } else if (state.owner != self()) {
            call.block(address);  // another guest thread holds it; retry once it unlocks
        } else if (state.type == 1) {
            if (state.count == 0xffffffffU) return "recursive mutex lock count overflow";
            ++state.count;
            call.set_result(0);
        } else if (state.type == 2) {
            call.set_result(edeadlk);
        } else {
            return threads_ ? "pthread_mutex_lock would deadlock: mutex is already held by the calling guest thread"
                            : "pthread_mutex_lock would deadlock: mutex is already held by the only guest thread";
        }
        return std::nullopt;
    });
    // int pthread_mutex_trylock(pthread_mutex_t*)
    dispatcher.bind("pthread_mutex_trylock", [this](ImportCall& call) -> std::optional<std::string> {
        const auto address = call.arg(0);
        if (address == 0) return "pthread_mutex_trylock on a null mutex";
        auto& state = mutexes_[address];
        if (state.count == 0) { state.count = 1; state.owner = self(); call.set_result(0); }
        else if (state.owner == self() && state.type == 1) {
            if (state.count == 0xffffffffU) return "recursive mutex lock count overflow";
            ++state.count;
            call.set_result(0);
        } else call.set_result(ebusy);
        return std::nullopt;
    });
    // int pthread_mutex_timedlock(pthread_mutex_t*, const timespec* abs_realtime)
    dispatcher.bind("pthread_mutex_timedlock", [this](ImportCall& call) -> std::optional<std::string> {
        const auto address = call.arg(0);
        if (address == 0) return "pthread_mutex_timedlock on a null mutex";
        if (!threads_) return "pthread_mutex_timedlock needs a guest thread scheduler";
        auto& state = mutexes_[address];
        auto& w = threads_->wait();
        if (state.count == 0 || (state.owner == self() && state.type == 1)) {
            w.timed_active = false;
            state.owner = self();
            if (state.count == 0xffffffffU) return "recursive mutex lock count overflow";
            ++state.count;
            call.set_result(0);
            return std::nullopt;
        }
        if (state.owner == self()) {
            w.timed_active = false;
            if (state.type == 2) { call.set_result(edeadlk); return std::nullopt; }
            return "pthread_mutex_timedlock would deadlock: mutex is already held by the calling guest thread";
        }
        if (!w.timed_active) {
            const int rc = threads_->deadline_from_timespec(call.runtime().memory, call.arg(1), 0, w.deadline);
            if (rc < 0) return "pthread_mutex_timedlock guest memory fault";
            if (rc != 0) { call.set_result(static_cast<std::uint32_t>(rc)); return std::nullopt; }
            w.timed_active = true;
        }
        if (threads_->now_ns() >= w.deadline) { w.timed_active = false; call.set_result(etimedout); }
        else call.block(address, w.deadline);
        return std::nullopt;
    });
    // int pthread_mutex_unlock(pthread_mutex_t*)
    dispatcher.bind("pthread_mutex_unlock", [this](ImportCall& call) -> std::optional<std::string> {
        const auto found = mutexes_.find(call.arg(0));
        if (found == mutexes_.end() || found->second.count == 0) {
            if (found != mutexes_.end() && found->second.type != 0) { call.set_result(eperm); return std::nullopt; }
            // Bionic's normal mutex has no owner or state check on unlock: unlocking an unlocked one stores "unlocked"
            // again and succeeds. The app does this (double unlock), so it is modelled as Bionic behaves.
            call.set_result(0);
            return std::nullopt;
        }
        // Bionic does not check the owner of a normal mutex; recursive and error-checking ones do.
        if (found->second.type != 0 && found->second.owner != self()) { call.set_result(eperm); return std::nullopt; }
        if (--found->second.count == 0) {
            found->second.owner = 0;
            if (threads_) threads_->wake(call.arg(0));
        }
        call.set_result(0);
        return std::nullopt;
    });
}

// ---- condition variables ----------------------------------------------------------------
std::optional<std::string> CondTable::wait(ImportCall& call, bool timed, std::optional<std::uint32_t> clock_override) {
    auto& memory = call.runtime().memory;
    auto& w = threads_.wait();
    if (w.cond_phase == 0) {
        const auto cond = call.arg(0), mutex = call.arg(1);
        if (cond == 0 || mutex == 0) { call.set_result(einval); return std::nullopt; }
        auto& state = conds_[cond];
        std::uint64_t deadline = 0;
        if (timed) {
            const int rc = threads_.deadline_from_timespec(memory, call.arg(2), clock_override.value_or(state.clock), deadline);
            if (rc < 0) return "pthread_cond_timedwait guest memory fault";
            if (rc != 0) { call.set_result(static_cast<std::uint32_t>(rc)); return std::nullopt; }
        }
        const auto count = mutexes_.release(mutex);
        if (count == 0) { call.set_result(eperm); return std::nullopt; }  // not held by the calling thread
        state.waiters.push_back(threads_.current_id());
        w.cond = cond;
        w.mutex = mutex;
        w.saved_count = count;
        w.signaled = w.timed_out = false;
        w.timed_active = timed;
        w.deadline = deadline;
        w.cond_phase = 1;
        call.block(cond, timed ? deadline : 0);
        return std::nullopt;
    }
    if (w.cond_phase == 1) {
        if (w.signaled) {
            w.cond_phase = 2;
        } else if (w.timed_active && threads_.now_ns() >= w.deadline) {
            auto& waiters = conds_[w.cond].waiters;
            std::erase(waiters, threads_.current_id());
            w.timed_out = true;
            w.cond_phase = 2;
        } else {
            call.block(w.cond, w.timed_active ? w.deadline : 0);
            return std::nullopt;
        }
    }
    // Phase 2: retake the mutex, which another thread may hold for now.
    if (!mutexes_.try_acquire(w.mutex, w.saved_count)) { call.block(w.mutex); return std::nullopt; }
    w.cond_phase = 0;
    w.timed_active = false;
    call.set_result(w.timed_out ? etimedout : 0);
    return std::nullopt;
}
void CondTable::install(ImportDispatcher& dispatcher) {
    dispatcher.bind("pthread_condattr_init", [](ImportCall& call) -> std::optional<std::string> {
        if (!write_word(call.runtime().memory, call.arg(0), 0)) return "pthread_condattr_init guest memory fault";
        call.set_result(0);
        return std::nullopt;
    });
    dispatcher.bind("pthread_condattr_destroy", [](ImportCall& call) -> std::optional<std::string> { call.set_result(0); return std::nullopt; });
    dispatcher.bind("pthread_condattr_setclock", [](ImportCall& call) -> std::optional<std::string> {
        if (call.arg(1) > 1) { call.set_result(einval); return std::nullopt; }
        if (!write_word(call.runtime().memory, call.arg(0), call.arg(1))) return "pthread_condattr_setclock guest memory fault";
        call.set_result(0);
        return std::nullopt;
    });
    dispatcher.bind("pthread_condattr_getclock", [](ImportCall& call) -> std::optional<std::string> {
        std::uint32_t clock = 0;
        if (!read_word(call.runtime().memory, call.arg(0), clock) || !write_word(call.runtime().memory, call.arg(1), clock))
            return "pthread_condattr_getclock guest memory fault";
        call.set_result(0);
        return std::nullopt;
    });
    // int pthread_cond_init(pthread_cond_t*, const pthread_condattr_t*)
    dispatcher.bind("pthread_cond_init", [this](ImportCall& call) -> std::optional<std::string> {
        std::uint32_t clock = 0;
        if (call.arg(0) == 0) return "pthread_cond_init on a null condition variable";
        if (call.arg(1) != 0 && !read_word(call.runtime().memory, call.arg(1), clock)) return "pthread_cond_init could not read the attribute";
        conds_[call.arg(0)] = {clock, {}};
        call.set_result(0);
        return std::nullopt;
    });
    dispatcher.bind("pthread_cond_destroy", [this](ImportCall& call) -> std::optional<std::string> {
        const auto found = conds_.find(call.arg(0));
        if (found != conds_.end() && !found->second.waiters.empty()) { call.set_result(ebusy); return std::nullopt; }
        conds_.erase(call.arg(0));
        call.set_result(0);
        return std::nullopt;
    });
    const auto wake = [this](ImportCall& call, bool all) -> std::optional<std::string> {
        const auto found = conds_.find(call.arg(0));
        if (found != conds_.end())
            while (!found->second.waiters.empty()) {
                const auto id = found->second.waiters.front();
                found->second.waiters.pop_front();
                if (auto* w = threads_.wait_of(id)) w->signaled = true;
                threads_.make_ready(id);
                if (!all) break;
            }
        call.set_result(0);
        return std::nullopt;
    };
    dispatcher.bind("pthread_cond_signal", [wake](ImportCall& call) { return wake(call, false); });
    dispatcher.bind("pthread_cond_broadcast", [wake](ImportCall& call) { return wake(call, true); });
    dispatcher.bind("pthread_cond_wait", [this](ImportCall& call) { return wait(call, false, std::nullopt); });
    dispatcher.bind("pthread_cond_timedwait", [this](ImportCall& call) { return wait(call, true, std::nullopt); });
    dispatcher.bind("pthread_cond_timedwait_monotonic_np", [this](ImportCall& call) { return wait(call, true, 1U); });
}

// ---- semaphores -------------------------------------------------------------------------
void SemaphoreTable::install(ImportDispatcher& dispatcher) {
    constexpr std::uint32_t sem_value_max = 0x7fffffff;
    // int sem_init(sem_t*, int pshared, unsigned value)
    dispatcher.bind("sem_init", [this](ImportCall& call) -> std::optional<std::string> {
        if (call.arg(0) == 0) return "sem_init on a null semaphore";
        if (call.arg(1) != 0) return "sem_init with pshared != 0 is not implemented";
        if (call.arg(2) > sem_value_max) { call.set_result(0xffffffffU); return "sem_init value exceeds SEM_VALUE_MAX (errno EINVAL is not modelled)"; }
        counts_[call.arg(0)] = call.arg(2);
        call.set_result(0);
        return std::nullopt;
    });
    // int sem_destroy(sem_t*)
    dispatcher.bind("sem_destroy", [this](ImportCall& call) -> std::optional<std::string> {
        if (counts_.erase(call.arg(0)) == 0) return "sem_destroy of a semaphore that was not initialized";
        call.set_result(0);
        return std::nullopt;
    });
    // int sem_post(sem_t*)
    dispatcher.bind("sem_post", [this](ImportCall& call) -> std::optional<std::string> {
        const auto found = counts_.find(call.arg(0));
        if (found == counts_.end()) return "sem_post on a semaphore that was not initialized";
        if (found->second == sem_value_max) return "sem_post would overflow SEM_VALUE_MAX";
        ++found->second;
        if (threads_) threads_->wake(call.arg(0));
        call.set_result(0);
        return std::nullopt;
    });
    // int sem_wait(sem_t*)
    dispatcher.bind("sem_wait", [this](ImportCall& call) -> std::optional<std::string> {
        const auto found = counts_.find(call.arg(0));
        if (found == counts_.end()) return "sem_wait on a semaphore that was not initialized";
        if (found->second == 0) {
            if (!threads_) return "sem_wait would deadlock: count is zero and there is only one guest thread";
            call.block(call.arg(0));
            return std::nullopt;
        }
        --found->second;
        call.set_result(0);
        return std::nullopt;
    });
    // int sem_trywait(sem_t*): -1 with errno EAGAIN when the count is zero.
    dispatcher.bind("sem_trywait", [this](ImportCall& call) -> std::optional<std::string> {
        const auto found = counts_.find(call.arg(0));
        if (found == counts_.end()) return "sem_trywait on a semaphore that was not initialized";
        if (found->second == 0) {
            if (!threads_ || !threads_->set_errno(call.runtime().memory, eagain))
                return "sem_trywait on a zero count needs errno (no guest thread scheduler with an errno address)";
            call.set_result(0xffffffffU);
            return std::nullopt;
        }
        --found->second;
        call.set_result(0);
        return std::nullopt;
    });
    // int sem_timedwait(sem_t*, const timespec* abs_realtime): -1 with errno ETIMEDOUT.
    dispatcher.bind("sem_timedwait", [this](ImportCall& call) -> std::optional<std::string> {
        if (!threads_) return "sem_timedwait needs a guest thread scheduler";
        const auto found = counts_.find(call.arg(0));
        if (found == counts_.end()) return "sem_timedwait on a semaphore that was not initialized";
        auto& w = threads_->wait();
        if (found->second != 0) { --found->second; w.timed_active = false; call.set_result(0); return std::nullopt; }
        if (!w.timed_active) {
            const int rc = threads_->deadline_from_timespec(call.runtime().memory, call.arg(1), 0, w.deadline);
            if (rc < 0) return "sem_timedwait guest memory fault";
            if (rc != 0) {
                if (!threads_->set_errno(call.runtime().memory, static_cast<std::uint32_t>(rc)))
                    return "sem_timedwait with an invalid time (errno is not configured)";
                call.set_result(0xffffffffU);
                return std::nullopt;
            }
            w.timed_active = true;
        }
        if (threads_->now_ns() >= w.deadline) {
            w.timed_active = false;
            if (!threads_->set_errno(call.runtime().memory, etimedout)) return "sem_timedwait timed out but errno is not configured";
            call.set_result(0xffffffffU);
        } else {
            call.block(call.arg(0), w.deadline);
        }
        return std::nullopt;
    });
    // int sem_getvalue(sem_t*, int*)
    dispatcher.bind("sem_getvalue", [this](ImportCall& call) -> std::optional<std::string> {
        const auto found = counts_.find(call.arg(0));
        if (found == counts_.end()) return "sem_getvalue on a semaphore that was not initialized";
        if (!write_word(call.runtime().memory, call.arg(1), found->second)) return "sem_getvalue guest memory fault";
        call.set_result(0);
        return std::nullopt;
    });
}

// ---- thread-specific data ---------------------------------------------------------------
ThreadKeys::ThreadKeys(GuestThreads* threads) : threads_(threads) {
    if (threads_)
        threads_->set_exit_hook([this](std::uint32_t id) {
            std::vector<std::pair<std::uint32_t, std::uint32_t>> pending;
            auto found = values_.find(id);
            if (found == values_.end()) return pending;
            for (std::uint32_t i = 0; i < found->second.size() && i < keys_.size(); ++i)
                if (keys_[i].live && keys_[i].destructor != 0 && found->second[i] != 0) {
                    pending.emplace_back(keys_[i].destructor, found->second[i]);
                    found->second[i] = 0;  // POSIX: the value is cleared before its destructor runs
                }
            return pending;
        });
}
void ThreadKeys::install(ImportDispatcher& dispatcher) {
    // int pthread_key_create(pthread_key_t* key, void (*destructor)(void*))
    dispatcher.bind("pthread_key_create", [this](ImportCall& call) -> std::optional<std::string> {
        std::uint32_t index = 0;
        while (index < keys_.size() && keys_[index].live) ++index;
        if (index == max_keys) { call.set_result(eagain); return std::nullopt; }
        if (index == keys_.size()) keys_.emplace_back();
        if (!write_word(call.runtime().memory, call.arg(0), index)) return "pthread_key_create guest memory fault";
        keys_[index] = {true, call.arg(1)};
        for (auto& [id, values] : values_) if (index < values.size()) values[index] = 0;
        call.set_result(0);
        return std::nullopt;
    });
    // int pthread_key_delete(pthread_key_t)
    dispatcher.bind("pthread_key_delete", [this](ImportCall& call) -> std::optional<std::string> {
        if (call.arg(0) >= keys_.size() || !keys_[call.arg(0)].live) { call.set_result(einval); return std::nullopt; }
        keys_[call.arg(0)] = {};
        for (auto& [id, values] : values_) if (call.arg(0) < values.size()) values[call.arg(0)] = 0;
        call.set_result(0);
        return std::nullopt;
    });
    // void* pthread_getspecific(pthread_key_t): NULL for an invalid key, as in POSIX.
    dispatcher.bind("pthread_getspecific", [this](ImportCall& call) -> std::optional<std::string> {
        std::uint32_t value = 0;
        if (call.arg(0) < keys_.size() && keys_[call.arg(0)].live) {
            const auto found = values_.find(self());
            if (found != values_.end() && call.arg(0) < found->second.size()) value = found->second[call.arg(0)];
        }
        call.set_result(value);
        return std::nullopt;
    });
    // int pthread_setspecific(pthread_key_t, const void*)
    dispatcher.bind("pthread_setspecific", [this](ImportCall& call) -> std::optional<std::string> {
        if (call.arg(0) >= keys_.size() || !keys_[call.arg(0)].live) { call.set_result(einval); return std::nullopt; }
        auto& values = values_[self()];
        if (values.size() <= call.arg(0)) values.resize(call.arg(0) + 1, 0);
        values[call.arg(0)] = call.arg(1);
        call.set_result(0);
        return std::nullopt;
    });
}
}
