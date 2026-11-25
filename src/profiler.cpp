// SPDX-License-Identifier: GPL-2.0-or-later
#include "atlan/profiler.hpp"
#include <algorithm>
#include <cstring>
#include <ostream>
#if defined(__x86_64__)
#include <x86intrin.h>
#endif

namespace atlan {
namespace {
// A cheap monotonic counter; write() converts it to time against steady_clock.
[[nodiscard]] inline std::uint64_t ticks() noexcept {
#if defined(__aarch64__)
    std::uint64_t value;
    __asm__ volatile("mrs %0, cntvct_el0" : "=r"(value));
    return value;
#elif defined(__x86_64__)
    return __rdtsc();
#else
    return static_cast<std::uint64_t>(std::chrono::steady_clock::now().time_since_epoch().count());
#endif
}
[[nodiscard]] std::string hex(std::uint32_t value) {
    static constexpr char digits[] = "0123456789abcdef";
    std::string out(8, '0');
    for (int i = 7; i >= 0; --i, value >>= 4) out[static_cast<std::size_t>(i)] = digits[value & 15];
    return out;
}
}

GuestProfiler::GuestProfiler() : first_ticks_(ticks()), first_time_(std::chrono::steady_clock::now()) {
    functions_.push_back({0, "?"});  // code below every known start, and the host as a caller
    starts_.emplace_back(0, 0);
}

std::uint32_t GuestProfiler::intern(std::uint32_t address) {
    const auto at = std::lower_bound(starts_.begin(), starts_.end(), std::pair<std::uint32_t, std::uint32_t>{address, 0});
    if (at != starts_.end() && at->first == address) return at->second;
    const auto index = static_cast<std::uint32_t>(functions_.size());
    functions_.push_back({address, {}});
    starts_.insert(at, {address, index});
    ++version_;
    return index;
}
void GuestProfiler::add_function(std::uint32_t address, std::string name) {
    Function& function = functions_[intern(address & ~1U)];
    if (function.name.empty()) function.name = std::move(name);
}
void GuestProfiler::locate(Thread& thread, std::uint32_t pc) {
    auto after = std::upper_bound(starts_.begin(), starts_.end(), std::pair<std::uint32_t, std::uint32_t>{pc, UINT32_MAX});
    thread.high = after == starts_.end() ? UINT32_MAX : after->first;
    --after;  // starts_[0] is address 0
    thread.low = after->first;
    thread.function = after->second;
    thread.version = version_;
}
void GuestProfiler::push(Thread& thread, std::uint32_t function, std::uint32_t return_address, std::uint32_t sp) {
    const std::uint32_t caller = thread.stack.empty() ? 0 : thread.stack.back().function;
    Edge& edge = edges_[std::uint64_t{caller} << 32 | function];
    ++edge.calls;
    ++functions_[function].calls;
    if (thread.depth.size() <= function) thread.depth.resize(functions_.size() + 64);
    ++thread.depth[function];
    thread.stack.push_back({function, return_address, sp, thread.instructions, thread.ticks, &edge});
}
void GuestProfiler::pop(Thread& thread) {
    const Frame frame = thread.stack.back();
    thread.stack.pop_back();
    const std::uint64_t instructions = thread.instructions - frame.instructions;
    frame.edge->inclusive_instructions += instructions;
    if (--thread.depth[frame.function] != 0) return;
    functions_[frame.function].inclusive_instructions += instructions;
    functions_[frame.function].inclusive_ticks += thread.ticks - frame.ticks;
}
void GuestProfiler::settle() {
    const std::uint64_t now = ticks();
    if (running_ != nullptr) {
        running_->ticks += now - last_ticks_;
        functions_[running_->function].self_ticks += now - last_ticks_;
    }
    last_ticks_ = now;
}
void GuestProfiler::pause() {
    settle();
    running_ = nullptr;
}
void GuestProfiler::unwind(std::uint32_t id) {
    settle();
    Thread& thread = threads_[id];
    while (!thread.stack.empty()) pop(thread);
    thread.ran = false;
}

void GuestProfiler::begin(Runtime& runtime, std::uint32_t id, bool entered) {
    settle();
    Thread& thread = threads_[id];
    running_ = &thread;
    const std::uint32_t pc = runtime.pc();
    const auto lr = static_cast<std::uint32_t>(runtime.cpu.registers[14]) & ~1U;
    const auto sp = static_cast<std::uint32_t>(runtime.cpu.registers[13]);
    auto& stack = thread.stack;
    const auto returns_here = [&] { return !stack.empty() && pc == stack.back().return_address && sp >= stack.back().sp; };
    bool returned = false;
    if (!entered) {
        if (returns_here()) {
            pop(thread);
            returned = true;
        } else if (!stack.empty() && sp > stack.back().sp) {
            // Frames whose stack is gone: a return that was not seen, or a non-local exit.
            while (!stack.empty() && sp > stack.back().sp) pop(thread);
            if (returns_here()) { pop(thread); returned = true; }
        }
    }
    const bool outside = pc < thread.low || pc >= thread.high;
    if (entered || !thread.ran || (!returned && lr == thread.next_sequential && pc != thread.next_sequential)) {
        push(thread, intern(pc), lr, sp);
        locate(thread, pc);
    } else if (!returned && outside && !stack.empty() && lr == stack.back().return_address && sp == stack.back().sp) {
        // Left the function with the caller's return address and stack restored: a tail call.
        const Frame old = stack.back();
        pop(thread);
        push(thread, intern(pc), old.return_address, old.sp);
        locate(thread, pc);
    } else if (returned || outside || thread.version != version_) {
        locate(thread, pc);
    }
    thread.block_pc = pc;
    thread.block_thumb = runtime.thumb();
}
void GuestProfiler::end(Runtime& runtime, std::uint64_t retired) {
    if (running_ == nullptr) return;
    Thread& thread = *running_;
    thread.instructions += retired;
    functions_[thread.function].self_instructions += retired;
    thread.ran = true;
    std::uint32_t next = thread.block_pc;
    if (!thread.block_thumb) {
        next += static_cast<std::uint32_t>(4 * retired);
    } else {
        for (std::uint64_t i = 0; i < retired; ++i) {
            const std::byte* host = runtime.memory.translate(next, MemoryAccess::execute);
            if (host == nullptr) break;
            std::uint16_t first;
            std::memcpy(&first, host, 2);
            next += first >= 0xe800 ? 4 : 2;
        }
    }
    thread.next_sequential = next;
}

void GuestProfiler::reset() {
    settle();
    for (auto& function : functions_)
        function.calls = function.self_instructions = function.self_ticks = function.inclusive_instructions = function.inclusive_ticks = 0;
    for (auto& [key, edge] : edges_) edge = {};
    for (auto& [id, thread] : threads_)
        for (auto& frame : thread.stack) { frame.instructions = thread.instructions; frame.ticks = thread.ticks; }
    first_ticks_ = last_ticks_;
    first_time_ = std::chrono::steady_clock::now();
}
void GuestProfiler::write(std::ostream& out) {
    settle();
    const std::uint64_t elapsed_ticks = last_ticks_ - first_ticks_;
    const double elapsed_ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - first_time_).count();
    const double ms_per_tick = elapsed_ticks != 0 ? elapsed_ms / static_cast<double>(elapsed_ticks) : 0.0;
    std::vector<const Function*> order;
    for (const auto& function : functions_)
        if (function.calls != 0 || function.self_instructions != 0 || function.self_ticks != 0) order.push_back(&function);
    std::sort(order.begin(), order.end(), [](const Function* a, const Function* b) {
        return a->self_instructions != b->self_instructions ? a->self_instructions > b->self_instructions : a->self_ticks > b->self_ticks;
    });
    for (const Function* f : order)
        out << "F\t" << hex(f->address) << '\t' << (f->name.empty() ? "-" : f->name) << '\t' << f->calls << '\t'
            << f->self_instructions << '\t' << static_cast<double>(f->self_ticks) * ms_per_tick << '\t'
            << f->inclusive_instructions << '\t' << static_cast<double>(f->inclusive_ticks) * ms_per_tick << '\n';
    for (const auto& [key, edge] : edges_)
        if (edge.calls != 0)
            out << "E\t" << hex(functions_[key >> 32].address) << '\t' << hex(functions_[key & 0xffffffffU].address) << '\t'
                << edge.calls << '\t' << edge.inclusive_instructions << '\n';
}
std::string GuestProfiler::summary(std::size_t count) {
    std::vector<const Function*> order;
    for (const auto& function : functions_)
        if (function.self_instructions != 0) order.push_back(&function);
    count = std::min(count, order.size());
    std::partial_sort(order.begin(), order.begin() + static_cast<std::ptrdiff_t>(count), order.end(),
                      [](const Function* a, const Function* b) { return a->self_instructions > b->self_instructions; });
    std::string line;
    for (std::size_t i = 0; i < count; ++i)
        line += (i ? " " : "") + (order[i]->name.empty() ? "0x" + hex(order[i]->address) : order[i]->name) + "=" +
                std::to_string(order[i]->self_instructions) + "/" + std::to_string(order[i]->calls);
    return line;
}
}
