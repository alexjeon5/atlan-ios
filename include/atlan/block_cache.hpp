// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include "atlan/runtime.hpp"
#include <cstdint>
#include <memory>
#include <vector>

namespace atlan {
// Executes guest code from blocks of pre-decoded instructions. No host code is
// generated and no executable memory is allocated: a block is data that the
// interpreter walks, so the decoder runs once per block instead of once per
// retired instruction. Behaviour must stay identical to Runtime::step().
//
// A BlockRunner drives one Runtime and must be used from the same host thread.
// Cached blocks are dropped whenever Memory::generation() changes, which covers
// map() and protect(); guest stores cannot reach executable pages because the
// permission model has no writable-and-executable state.
class BlockRunner {
public:
    struct Stats {
        std::uint64_t blocks_built{};
        std::uint64_t block_entries{};  // lookups that found a cached block
        std::uint64_t slow_steps{};     // instructions left to Runtime::step()
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
    [[nodiscard]] const Stats& stats() const noexcept { return stats_; }

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
    Stats stats_;
    Runtime& runtime_;
};
}
