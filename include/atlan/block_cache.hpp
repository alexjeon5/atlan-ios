// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include "atlan/runtime.hpp"
#include <cstdint>
#include <memory>
#include <string>
#include <utility>
#include <vector>

namespace atlan {
// Executes guest code from blocks of pre-decoded instructions. No host code is
// generated and no executable memory is allocated: a block is data that the
// interpreter walks, so the decoder runs once per block instead of once per
// retired instruction. Behaviour must stay identical to Runtime::step().
//
// A BlockRunner drives one Runtime and must be used from the same host thread.
// Cached blocks are dropped whenever Memory::code_generation() changes, which
// covers every map(), unmap() and protect() that involves an executable page;
// guest stores cannot reach executable pages because the permission model has
// no writable-and-executable state.
// A host function called straight from the block loop instead of through the import dispatcher: a replaced guest
// function (ImportDispatcher::replace_function_inline) or an import (ImportDispatcher::bind_inline). It may read
// and write core registers and guest memory, and must not block, call back into the guest or change the PC.
// `run` returns false, before it changes any state, to decline: the loop then reports the trap (the undefined
// instruction, or the import's execute fault), and the dispatcher's service for the same function handles the call.
using InlineRun = bool (*)(CpuState& cpu, Memory& memory) noexcept;
struct InlineFunction {
    InlineRun run{};
    std::uint64_t calls{};  // calls `run` completed; the dispatcher folds them into its import counts
};

class BlockRunner {
public:
    struct Stats {
        std::uint64_t blocks_built{};
        std::uint64_t block_entries{};  // lookups that found a cached block
        std::uint64_t slow_steps{};     // instructions left to Runtime::step()
        std::uint64_t generic{};        // instructions run by the library handlers (no fast handler)
        std::uint64_t fallbacks{};      // fast handlers that handed over (fault, page-crossing access)
        std::uint64_t flushes{};
    };
    explicit BlockRunner(Runtime& runtime);
    ~BlockRunner();
    BlockRunner(const BlockRunner&) = delete;
    BlockRunner& operator=(const BlockRunner&) = delete;

    // Same contract as Runtime::run(): stops at the first event other than
    // retired, or when `budget` instructions have retired.
    [[nodiscard]] Result run(std::uint64_t budget) noexcept;
    // Executes at most `budget` instructions of the block at the current PC and
    // returns. Event::retired means the block ended normally. A caller that
    // inspects the PC between instructions (import traps, call return targets)
    // may call this instead of Runtime::step() because a block never runs past
    // a PC change.
    [[nodiscard]] Result step_block(std::uint64_t budget) noexcept;
    void flush() noexcept;
    // Calls `function` (which must outlive the runner) where a block starts with the replaced-function trap at
    // `key`, or where execution reaches the import trap `key` (a PC that is not executable); null removes it.
    // Drops the cached blocks.
    void set_inline(std::uint64_t key, InlineFunction* function);
    [[nodiscard]] const Stats& stats() const noexcept { return stats_; }
    // Diagnostics: how often each instruction form ran without a fast handler, most frequent first.
    [[nodiscard]] std::vector<std::pair<std::string, std::uint64_t>> generic_forms() const;

private:
    template <bool Single>
    [[nodiscard]] Result execute(std::uint64_t budget) noexcept;
    struct Impl;
    std::unique_ptr<Impl> impl_;
    Stats stats_;
    Runtime& runtime_;
};
}
