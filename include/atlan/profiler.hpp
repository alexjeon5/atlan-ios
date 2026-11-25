// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include "atlan/runtime.hpp"
#include <chrono>
#include <cstdint>
#include <iosfwd>
#include <string>
#include <unordered_map>
#include <vector>

namespace atlan {
// Attributes guest execution to guest functions: calls, instructions and host time, each as "self" (spent in
// the function's own code) and "inclusive" (until it returns, callees and host services included), plus who
// called whom. A function is an address the embedder named (exported symbols, import traps) or one the guest
// was seen calling; code is attributed to the nearest function start at or below it.
//
// The ImportDispatcher drives it block by block (ImportDispatcher::set_profiler), which costs the block
// chaining, so instruction counts are exact while host times carry the profiling overhead: compare them with
// each other, not with an unprofiled run. Self figures only depend on the function starts. Inclusive figures
// and call counts follow a shadow call stack (calls: the link register holds the address after the block just
// run; returns: the PC is the frame's return address) and are best effort around longjmp-like transfers.
class GuestProfiler {
public:
    GuestProfiler();
    // Names the function starting at `address` (bit 0 ignored). The first name of an address wins.
    void add_function(std::uint32_t address, std::string name);

    // Before a block of thread `thread` runs. `entered`: the host itself started this call (ImportDispatcher::call,
    // call_back), so the PC is a function start however the registers look.
    void begin(Runtime& runtime, std::uint32_t thread, bool entered);
    // After that block: `retired` instructions ran, sequentially from the PC begin() saw.
    void end(Runtime& runtime, std::uint64_t retired);
    // The host abandoned whatever `thread` was running (a new top-level call reuses the stack).
    void unwind(std::uint32_t thread);
    // No guest code runs until the next begin(); the host time in between belongs to no function.
    void pause();

    // Zeroes every counter; names and discovered functions stay.
    void reset();
    // Tab-separated, sorted by self instructions:
    //   F address name calls self_instructions self_ms inclusive_instructions inclusive_ms
    //   E caller_address callee_address calls inclusive_instructions     (call edges)
    void write(std::ostream& out);
    // One line: the `count` busiest functions by self instructions, for a log.
    [[nodiscard]] std::string summary(std::size_t count);

private:
    struct Function {
        std::uint32_t address{};
        std::string name;
        std::uint64_t calls{}, self_instructions{}, self_ticks{}, inclusive_instructions{}, inclusive_ticks{};
    };
    struct Edge { std::uint64_t calls{}, inclusive_instructions{}; };
    struct Frame {
        std::uint32_t function{}, return_address{}, sp{};
        std::uint64_t instructions{}, ticks{};  // the thread's clocks at entry
        Edge* edge{};
    };
    struct Thread {
        std::vector<Frame> stack;
        std::vector<std::uint32_t> depth;      // per function: frames on `stack`, so recursion is counted once
        std::uint64_t instructions{}, ticks{};  // clocks that only advance while this thread runs
        std::uint32_t function{};               // the function the current block is attributed to
        std::uint32_t low{}, high{};            // its address range
        std::uint32_t block_pc{}, next_sequential{};
        bool block_thumb{}, ran{};
        std::uint64_t version{};                // starts_ version `low`/`high` were taken from
    };
    [[nodiscard]] std::uint32_t intern(std::uint32_t address);      // exact start, created if unknown
    void locate(Thread& thread, std::uint32_t pc);                  // sets function, low, high
    void push(Thread& thread, std::uint32_t function, std::uint32_t return_address, std::uint32_t sp);
    void pop(Thread& thread);
    void settle();  // charges the host time since the last call to the thread that was running

    std::vector<Function> functions_;
    std::vector<std::pair<std::uint32_t, std::uint32_t>> starts_;  // (address, index into functions_), sorted
    std::unordered_map<std::uint64_t, Edge> edges_;                // caller index << 32 | callee index
    std::unordered_map<std::uint32_t, Thread> threads_;
    Thread* running_{};
    std::uint64_t version_{1};
    std::uint64_t last_ticks_{};
    std::uint64_t first_ticks_{};
    std::chrono::steady_clock::time_point first_time_;
};
}
