// SPDX-License-Identifier: GPL-2.0-or-later
#include "atlan/block_cache.hpp"
#include "atlan/vfp.hpp"
#include "cpu/arm64/aarch32/guest_key.hpp"
#include "cpu/arm64/aarch32/semantics.hpp"
#include <array>
#include <cstring>

namespace atlan {
namespace a32 = uml::cpu::arm64::aarch32;
namespace {
constexpr std::size_t max_block = 64;       // instructions per block
constexpr std::size_t slot_count = 4096;    // direct-mapped lookup table
constexpr std::size_t max_blocks = 8192;    // flush everything beyond this

struct Entry {
    a32::Instruction insn;
    a32::PreparedInstruction prepared;  // resolved handler, null when execute_a32() must report
    std::uint64_t next_key;  // PC key after sequential execution
    std::uint32_t pc;
    std::uint32_t word;      // A32 word, for the VFP route
    bool route_vfp;          // offer the word to the VFP executor first
};
struct Block {
    std::uint64_t key;
    std::uint8_t itstate;
    std::vector<Entry> entries;
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

    void flush() noexcept {
        store.clear();
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
            block->entries.push_back({insn, a32::prepare_a32(insn), thumb ? a32::t32_key(next) : a32::a32_key(next), pc, word, route});
            constexpr auto ends = a32::trait::branch | a32::trait::interwork | a32::trait::alu_pc |
                                  a32::trait::load_pc | a32::trait::exception;
            if (!insn.legal() || (info.traits & ends) != 0) break;
            it = insn.next_it.bits();
            pc = next;
        }
        if (block->entries.empty()) return nullptr;
        if (store.size() >= max_blocks) flush();
        Block* raw = block.get();
        store.push_back(std::move(block));
        return raw;
    }
};

BlockRunner::BlockRunner(Runtime& runtime) : impl_(std::make_unique<Impl>(runtime)), runtime_(runtime) {}
BlockRunner::~BlockRunner() = default;
void BlockRunner::flush() noexcept { impl_->flush(); ++stats_.flushes; }

Result BlockRunner::step_block(std::uint64_t budget) noexcept {
    auto& d = *impl_;
    if (budget == 0) return {Event::budget_exhausted, 0};
    if (d.generation != d.memory.generation()) {
        d.generation = d.memory.generation();
        flush();
    }
    const std::uint64_t key = d.cpu.pc;
    const std::uint8_t it = d.cpu.a32_itstate;
    Block* block = nullptr;
    if (a32::key_is_canonical(key)) {
        Block*& slot = d.slots[slot_of(key, it)];
        if (slot && slot->key == key && slot->itstate == it) {
            block = slot;
            ++stats_.block_entries;
        } else {
            try { block = d.build(key, it); } catch (...) { block = nullptr; }
            // build() may have flushed the table; re-resolve the slot.
            if (block) {
                d.slots[slot_of(key, it)] = block;
                ++stats_.blocks_built;
            }
        }
    }
    if (!block) {
        ++stats_.slow_steps;
        return runtime_.step();
    }
    std::uint64_t retired = 0;
    const std::size_t count = static_cast<std::size_t>(std::min<std::uint64_t>(block->entries.size(), budget));
    for (std::size_t i = 0; i < count; ++i) {
        const Entry& e = block->entries[i];
        if (e.route_vfp) {
            const auto done = vfp::execute_a32(d.cpu, d.memory, e.word, e.pc);
            switch (done.kind) {
            case vfp::Execution::Kind::not_vfp: break;
            case vfp::Execution::Kind::retired:
                ++retired;
                if (d.cpu.pc != e.next_key) return {Event::retired, retired};
                continue;
            case vfp::Execution::Kind::fault:
                return {Event::memory_fault, retired, done.address, 0, done.access, done.alignment};
            case vfp::Execution::Kind::unsupported:
                return {Event::unsupported_instruction, retired, e.pc, e.word, MemoryAccess::execute, false, done.reason};
            }
        }
        const auto result = e.prepared.run ? e.prepared.run(d.cpu, e.insn, d.adapter, e.prepared.info)
                                           : a32::execute_a32(d.cpu, e.insn, d.adapter);
        if (result.event != a32::StepEvent::retired) {
            const Event event = map_event(result.event);
            if (event == Event::supervisor_call) ++retired;
            return {event, retired, result.fault_address, result.syndrome, result.access, result.alignment};
        }
        ++retired;
        if (d.cpu.pc != e.next_key) break;
    }
    return {Event::retired, retired};
}

Result BlockRunner::run(std::uint64_t budget) noexcept {
    std::uint64_t retired = 0;
    while (retired < budget) {
        auto result = step_block(budget - retired);
        retired += result.retired;
        if (result.event != Event::retired) {
            result.retired = retired;
            return result;
        }
    }
    return {Event::budget_exhausted, retired};
}
}
