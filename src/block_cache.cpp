// SPDX-License-Identifier: GPL-2.0-or-later
#include "atlan/block_cache.hpp"
#include "atlan/vfp.hpp"
#include "cpu/arm64/aarch32/guest_key.hpp"
#include "cpu/arm64/aarch32/semantics.hpp"
#include <algorithm>
#include <array>
#include <cstring>

namespace atlan {
namespace a32 = uml::cpu::arm64::aarch32;
namespace {
constexpr std::size_t max_block = 64;        // instructions per block
constexpr std::size_t slot_count = 16384;    // direct-mapped lookup table
constexpr std::size_t max_entries = 1U << 17;  // cached instructions; flush everything beyond this

struct Entry {
    a32::Instruction insn;
    a32::PreparedInstruction prepared;  // resolved handler, null when execute_a32() must report
    std::uint64_t next_key;  // PC key after sequential execution
    std::uint32_t pc;
    std::uint32_t word;      // A32 word, for the VFP route
    bool route_vfp;          // offer the word to the VFP executor first
};
struct Block;
// The successor a block last continued into, so a repeated transfer skips the table lookup. Links die with
// the blocks: a flush destroys every block at once.
struct Link {
    std::uint64_t key{};
    Block* block{};
    std::uint8_t itstate{};
};
struct Block {
    std::uint64_t key;
    std::uint8_t itstate;
    // One per instruction, dense for the execution loop. Where `run` is null the instruction has no fast
    // handler and `imm` indexes `entries`, which holds the full decode of only those instructions.
    std::vector<a32::FastOp> fast;
    std::vector<Entry> entries;
    std::array<Link, 2> links{};
};

class Adapter final : public a32::A32Memory {
public:
    Adapter(Memory& memory, CpuState& cpu) : memory_(memory), cpu_(cpu) {}
    std::byte* translate(std::uint64_t address, MemoryAccess access) noexcept override {
        return memory_.translate(address, access);
    }
    void stored(std::byte*, std::size_t) noexcept override { cpu_.clear_exclusive_monitor(); }
private:
    Memory& memory_;
    CpuState& cpu_;
};

[[nodiscard]] Event map_event(a32::StepEvent event) noexcept {
    switch (event) {
    case a32::StepEvent::retired: return Event::retired;
    case a32::StepEvent::supervisor_call: return Event::supervisor_call;
    case a32::StepEvent::breakpoint: return Event::breakpoint;
    case a32::StepEvent::undefined: return Event::undefined_instruction;
    case a32::StepEvent::memory_fault: return Event::memory_fault;
    case a32::StepEvent::pc_alignment: return Event::pc_alignment;
    default: return Event::unsupported_instruction;
    }
}
[[nodiscard]] std::size_t slot_of(std::uint64_t key, std::uint8_t itstate) noexcept {
    const std::uint64_t h = (key ^ (key >> 13) ^ (std::uint64_t{itstate} << 7)) * 0x9e3779b97f4a7c15ULL;
    return static_cast<std::size_t>(h >> 40) & (slot_count - 1);
}
}

struct BlockRunner::Impl {
    Impl(Runtime& runtime) : memory(runtime.memory), cpu(runtime.cpu), adapter(memory, cpu),
                             profile(a32::FeatureSet{}), slots(slot_count, nullptr) {}
    Memory& memory;
    CpuState& cpu;
    Adapter adapter;
    a32::Profile profile;
    std::uint64_t generation{};
    std::vector<std::unique_ptr<Block>> store;
    std::vector<Block*> slots;
    std::size_t entries{};  // instructions held by `store`
    std::vector<std::uint64_t> generic_forms = std::vector<std::uint64_t>(a32::kFormCount);

    void flush() noexcept {
        store.clear();
        entries = 0;
        std::fill(slots.begin(), slots.end(), nullptr);
    }
    // Returns nullptr when not even one instruction can be fetched and decoded
    // into a block; the caller then runs Runtime::step(), which reports why.
    Block* build(std::uint64_t key, std::uint8_t itstate) {
        const bool thumb = a32::key_is_thumb(key);
        auto block = std::make_unique<Block>();
        block->key = key;
        block->itstate = itstate;
        std::uint32_t pc = a32::key_pc(key);
        std::uint8_t it = itstate;
        for (std::size_t i = 0; i < max_block; ++i) {
            a32::Instruction insn;
            std::uint32_t word = 0;
            std::uint32_t length = 4;
            const std::byte* host = memory.translate(pc, MemoryAccess::execute);
            if (!host) break;
            if (!thumb) {
                std::memcpy(&word, host, 4);
                insn = a32::decode_a32(word, profile);
            } else {
                std::uint16_t first{}, second{};
                std::memcpy(&first, host, 2);
                const bool wide = a32::thumb_length(first) == 4U;
                if (wide) {
                    const std::byte* tail = memory.translate(pc + 2U, MemoryAccess::execute);
                    if (!tail) break;
                    std::memcpy(&second, tail, 2);
                }
                length = wide ? 4 : 2;
                insn = a32::decode_thumb(wide ? std::uint32_t{first} << 16 | second : std::uint32_t{first} << 16,
                                         a32::ItState{it}, profile);
            }
            const auto& info = insn.info();
            const bool route = !thumb && (!insn.legal() || !a32::a32_semantics_covers(insn.form) ||
                                          (info.traits & a32::trait::fp_state) != 0 ||
                                          info.check != a32::Check::none);
            const std::uint32_t next = pc + length;
            const std::uint64_t next_key = thumb ? a32::t32_key(next) : a32::a32_key(next);
            a32::FastOp fast{};
            if (route || !a32::prepare_fast(insn, thumb ? a32::t32_key(pc) : a32::a32_key(pc), fast)) {
                fast = {};
                fast.next_key = next_key;
                fast.imm = static_cast<std::uint32_t>(block->entries.size());
                block->entries.push_back({insn, a32::prepare_a32(insn), next_key, pc, word, route});
            }
            block->fast.push_back(fast);
            // Execution leaves a block as soon as the PC is not the next entry's, so where a block ends here only
            // decides how much is decoded ahead. The traits are per form ("may write the PC"), not per
            // instruction: a conditional branch or an ALU/load form that might target the PC continues, and the
            // block stops at an exception or at an unconditional transfer.
            if (!insn.legal() || (info.traits & a32::trait::exception) != 0) break;
            const bool always = insn.cond == 14 && !insn.in_it;
            if (always && (info.traits & (a32::trait::branch | a32::trait::interwork)) != 0) break;
            if (always && (info.traits & a32::trait::alu_pc) != 0 && insn.operands.d == 15) break;
            if (always && (info.traits & a32::trait::load_pc) != 0 &&
                (insn.operands.t == 15 || (insn.operands.registers & 0x8000U) != 0)) break;
            it = insn.next_it.bits();
            pc = next;
        }
        if (block->fast.empty()) return nullptr;
        if (entries + block->fast.size() > max_entries) flush();
        entries += block->fast.size();
        Block* raw = block.get();
        store.push_back(std::move(block));
        return raw;
    }
};

BlockRunner::BlockRunner(Runtime& runtime) : impl_(std::make_unique<Impl>(runtime)), runtime_(runtime) {}
BlockRunner::~BlockRunner() = default;
void BlockRunner::flush() noexcept { impl_->flush(); ++stats_.flushes; }

// Runs blocks until an event, the budget, or (when `single`) the end of the first block.
Result BlockRunner::execute(std::uint64_t budget, bool single) noexcept {
    auto& d = *impl_;
    if (budget == 0) return {Event::budget_exhausted, 0};
    if (d.generation != d.memory.code_generation()) {
        d.generation = d.memory.code_generation();
        flush();
    }
    std::uint64_t retired = 0;
    Block* previous = nullptr;
    while (true) {
        const std::uint64_t key = d.cpu.pc;
        const std::uint8_t it = d.cpu.a32_itstate;
        Block* block = nullptr;
        if (previous != nullptr) {
            for (const Link& link : previous->links)
                if (link.block != nullptr && link.key == key && link.itstate == it) { block = link.block; break; }
        }
        if (block != nullptr) {
            ++stats_.block_entries;
        } else if (a32::key_is_canonical(key)) {
            Block*& slot = d.slots[slot_of(key, it)];
            if (slot && slot->key == key && slot->itstate == it) {
                block = slot;
                ++stats_.block_entries;
            } else if (d.memory.translate(a32::key_pc(key), MemoryAccess::execute) == nullptr) {
                // Not executable (an import trap, a return sentinel): Runtime::step() below reports the fault
                // without a block being allocated for nothing.
            } else {
                const std::uint64_t flushes = stats_.flushes;
                try { block = d.build(key, it); } catch (...) { block = nullptr; }
                // build() may have flushed the table; re-resolve the slot and forget the destroyed predecessor.
                if (stats_.flushes != flushes || d.store.size() <= 1) previous = nullptr;
                if (block) {
                    d.slots[slot_of(key, it)] = block;
                    ++stats_.blocks_built;
                }
            }
            if (block != nullptr && previous != nullptr) {
                Link& link = previous->links[0].block == nullptr ? previous->links[0] : previous->links[1];
                link = {key, block, it};
            }
        }
        if (!block) {
            ++stats_.slow_steps;
            auto result = runtime_.step();
            retired += result.retired;
            if (result.event != Event::retired) { result.retired = retired; return result; }
            if (single) return {Event::retired, retired};
            if (retired >= budget) return {Event::budget_exhausted, retired};
            previous = nullptr;
            continue;
        }
        const std::size_t count = static_cast<std::size_t>(std::min<std::uint64_t>(block->fast.size(), budget - retired));
        const a32::FastOp* const fast = block->fast.data();
        for (std::size_t i = 0; i < count; ++i) {
            if (fast[i].run != nullptr) [[likely]] {
                const unsigned done = fast[i].run(d.cpu, fast[i], d.memory);
                if (done == a32::fast_next) [[likely]] { ++retired; continue; }
                if (done == a32::fast_jump) { ++retired; break; }
                // fast_fallback: nothing changed. Runtime::step() executes the instruction or reports its event.
                ++stats_.fallbacks;
                auto result = runtime_.step();
                retired += result.retired;
                if (result.event != Event::retired) { result.retired = retired; return result; }
                if (d.cpu.pc != fast[i].next_key) break;
                continue;
            }
            const Entry* const e = &block->entries[fast[i].imm];
            ++stats_.generic;
            ++d.generic_forms[static_cast<std::size_t>(e->insn.form)];
            if (e->route_vfp) {
                const auto done = vfp::execute_a32(d.cpu, d.memory, e->word, e->pc);
                if (done.kind == vfp::Execution::Kind::retired) {
                    ++retired;
                    if (d.cpu.pc != e->next_key) break;
                    continue;
                }
                if (done.kind == vfp::Execution::Kind::fault)
                    return {Event::memory_fault, retired, done.address, 0, done.access, done.alignment};
                if (done.kind == vfp::Execution::Kind::unsupported)
                    return {Event::unsupported_instruction, retired, e->pc, e->word, MemoryAccess::execute, false, done.reason};
            }
            const auto result = e->prepared.run ? e->prepared.run(d.cpu, e->insn, d.adapter, e->prepared.info)
                                                : a32::execute_a32(d.cpu, e->insn, d.adapter);
            if (result.event != a32::StepEvent::retired) [[unlikely]] {
                const Event event = map_event(result.event);
                if (event == Event::supervisor_call) ++retired;
                return {event, retired, result.fault_address, result.syndrome, result.access, result.alignment};
            }
            ++retired;
            if (d.cpu.pc != e->next_key) break;
        }
        if (single) return {Event::retired, retired};
        if (retired >= budget) return {Event::budget_exhausted, retired};
        previous = block;
    }
}

std::vector<std::pair<std::string, std::uint64_t>> BlockRunner::generic_forms() const {
    std::vector<std::pair<std::string, std::uint64_t>> out;
    for (std::size_t i = 0; i < impl_->generic_forms.size(); ++i)
        if (impl_->generic_forms[i] != 0) out.emplace_back(a32::kForms[i].name, impl_->generic_forms[i]);
    std::sort(out.begin(), out.end(), [](const auto& a, const auto& b) { return a.second > b.second; });
    return out;
}
Result BlockRunner::step_block(std::uint64_t budget) noexcept { return execute(budget, true); }
Result BlockRunner::run(std::uint64_t budget) noexcept { return execute(budget, false); }
}
