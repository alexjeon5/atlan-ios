// SPDX-License-Identifier: GPL-2.0-or-later
#include "atlan/block_cache.hpp"
#include "atlan/vfp.hpp"
#include "cpu/arm64/aarch32/guest_key.hpp"
#include "cpu/arm64/aarch32/semantics.hpp"
#include <algorithm>
#include <array>
#include <cstring>
#include <unordered_map>

namespace atlan {
namespace a32 = uml::cpu::arm64::aarch32;
namespace {
constexpr std::size_t max_block = 64;        // instructions per block
constexpr std::size_t slot_count = 65536;    // direct-mapped lookup table
// Cached instructions; everything is flushed beyond this. A block runs on past conditional branches, so code reached
// from several entry points is held once per entry: a map screen's working set exceeded 2^17 and rebuilt ~36,000
// blocks a second. At 2^19 one full cache is ~25 MB of FastOps.
constexpr std::size_t max_entries = 1U << 19;

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
    std::uint64_t key{~std::uint64_t{0}};  // never a canonical key while unused
    Block* block{};
    std::uint8_t itstate{};
};
struct Block {
    std::uint64_t key;
    std::uint8_t itstate;
    // One per instruction, dense for the execution loop. Where `run` is generic_marker the instruction has no fast
    // handler and `imm` indexes `entries`, which holds the full decode of only those instructions.
    std::vector<a32::FastOp> fast;
    std::vector<Entry> entries;
    std::array<Link, 2> links{};
    std::size_t length{};  // fast.size(), kept so the loop does not divide by sizeof(FastOp)
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

// The handler of an instruction without a fast one: hands it back to the block loop.
unsigned generic_marker(CpuState&, const a32::FastOp&, Memory&) noexcept { return a32::fast_generic; }
// UDF #0, the instruction the import dispatcher writes over the start of a replaced guest function. It is reported
// straight from the block loop instead of through the generic decoder, because replaced functions are called often.
constexpr unsigned fast_undefined_trap = 4;
// An inline function returned to its caller. Like the import dispatcher's call of a replaced function, it retires
// no guest instruction.
constexpr unsigned fast_inline_return = 5;
constexpr std::uint32_t udf_arm = 0xe7f000f0, udf_thumb = 0xde00;
unsigned undefined_trap(CpuState&, const a32::FastOp&, Memory&) noexcept { return fast_undefined_trap; }
// A PC that cannot be fetched (an import trap, a return sentinel): the execute fault Runtime::step() reports. Such
// a PC gets a block of this one op, so a repeated import call finds it like any other block.
constexpr unsigned fast_fetch_fault = 6;
unsigned fetch_fault(CpuState&, const a32::FastOp&, Memory&) noexcept { return fast_fetch_fault; }
// An inline function at the trap; `key` holds its InlineFunction. Returns to LR as BX LR does. When the function
// declines, the trap is reported: `Declined` is the undefined instruction of a replaced function or the fetch fault
// of an import.
template <unsigned Declined>
unsigned inline_call(CpuState& cpu, const a32::FastOp& op, Memory& memory) noexcept {
    auto* function = reinterpret_cast<InlineFunction*>(static_cast<std::uintptr_t>(op.key));
    if (!function->run(cpu, memory)) return Declined;
    ++function->calls;
    cpu.pc = a32::key_from_target(static_cast<std::uint32_t>(cpu.registers[14]));
    cpu.a32_itstate = 0;
    return fast_inline_return;
}
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

[[nodiscard]] std::uint32_t insn_halfword(const std::byte* host) noexcept {
    std::uint16_t value{};
    std::memcpy(&value, host, 2);
    return value;
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
    std::uint64_t capacity_flushes{};
    std::vector<std::uint64_t> generic_forms = std::vector<std::uint64_t>(a32::kFormCount);
    std::unordered_map<std::uint64_t, InlineFunction*> inlines;  // by the key of the function's first instruction

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
            if (thumb ? (it == 0 && length == 2 && insn_halfword(host) == udf_thumb) : word == udf_arm) {
                fast.run = undefined_trap;
                fast.next_key = next_key;
                fast.imm = thumb ? udf_thumb : udf_arm;
                if (i == 0) {
                    if (const auto found = inlines.find(key); found != inlines.end()) {
                        fast.run = inline_call<fast_undefined_trap>;
                        fast.key = reinterpret_cast<std::uintptr_t>(found->second);
                    }
                }
                block->fast.push_back(fast);
                break;
            }
            // A routed word is offered to the VFP executor first, so only its handlers may take it.
            if (route ? !vfp::prepare_fast(word, pc, fast)
                      : !a32::prepare_fast(insn, thumb ? a32::t32_key(pc) : a32::a32_key(pc), fast)) {
                fast = {};
                fast.run = generic_marker;
                fast.next_key = next_key;
                fast.next_it = insn.next_it.bits();
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
        return keep(std::move(block));
    }
    // The one-op block of a PC that is not executable: the inline function bound to it, or the fetch fault.
    Block* build_trap(std::uint64_t key, std::uint8_t itstate) {
        auto block = std::make_unique<Block>();
        block->key = key;
        block->itstate = itstate;
        a32::FastOp fast{};
        fast.run = fetch_fault;
        fast.next_key = key;
        fast.next_it = itstate;
        if (const auto found = inlines.find(key); found != inlines.end()) {
            fast.run = inline_call<fast_fetch_fault>;
            fast.key = reinterpret_cast<std::uintptr_t>(found->second);
        }
        block->fast.push_back(fast);
        return keep(std::move(block));
    }
    Block* keep(std::unique_ptr<Block> block) {
        block->length = block->fast.size();
        block->fast.shrink_to_fit();
        if (entries + block->fast.size() > max_entries) { flush(); ++capacity_flushes; }
        entries += block->fast.size();
        Block* raw = block.get();
        store.push_back(std::move(block));
        return raw;
    }
};

BlockRunner::BlockRunner(Runtime& runtime) : impl_(std::make_unique<Impl>(runtime)), runtime_(runtime) {}
BlockRunner::~BlockRunner() = default;
void BlockRunner::flush() noexcept { impl_->flush(); ++stats_.flushes; }
void BlockRunner::set_inline(std::uint64_t key, InlineFunction* function) {
    if (function != nullptr) impl_->inlines[key] = function;
    else impl_->inlines.erase(key);
    flush();
}

// Runs blocks until an event, the budget, or (when `Single`) the end of the first block.
template <bool Single>
Result BlockRunner::execute(std::uint64_t budget) noexcept {
    auto& d = *impl_;
    if (budget == 0) return {Event::budget_exhausted, 0};
    if (d.generation != d.memory.code_generation()) {
        d.generation = d.memory.code_generation();
        flush();
    }
    std::uint64_t retired = 0;
    Block* previous = nullptr;
    // Block entries are counted in a register and added to the statistics once, when the run ends.
    struct Entries {
        std::uint64_t& total;
        std::uint64_t count{};
        ~Entries() { total += count; }
    } entries{stats_.block_entries};
    // Locals, so the handler calls do not force reloads through `d`.
    CpuState& cpu = d.cpu;
    Memory& memory = d.memory;
    while (true) {
        const std::uint64_t key = cpu.pc;
        const std::uint8_t it = cpu.a32_itstate;
        Block* block = nullptr;
        if (previous != nullptr) {
            for (const Link& link : previous->links)
                if (link.key == key && link.itstate == it) { block = link.block; break; }
        }
        if (block != nullptr) {
            ++entries.count;
        } else if (a32::key_is_canonical(key)) {
            Block*& slot = d.slots[slot_of(key, it)];
            if (slot && slot->key == key && slot->itstate == it) {
                block = slot;
                ++entries.count;
            } else {
                const std::uint64_t flushes = stats_.flushes;
                const bool fetches = d.memory.translate(a32::key_pc(key), MemoryAccess::execute) != nullptr;
                try { block = fetches ? d.build(key, it) : d.build_trap(key, it); } catch (...) { block = nullptr; }
                // build() may have flushed the table; re-resolve the slot and forget the destroyed predecessor.
                if (stats_.flushes != flushes || d.store.size() <= 1) previous = nullptr;
                if (block) {
                    d.slots[slot_of(key, it)] = block;
                    ++stats_.blocks_built;
                    stats_.capacity_flushes = d.capacity_flushes;
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
            if (Single) return {Event::retired, retired};
            if (retired >= budget) return {Event::budget_exhausted, retired};
            previous = nullptr;
            continue;
        }
        // Fast handlers do not store the PC or ITSTATE; they follow from the position in the block. The state
        // is written back where the run leaves the sequence: at its end, before a slow instruction (which may
        // read the PC or report it), and by a handler that transfers control.
        const std::size_t count = static_cast<std::size_t>(std::min<std::uint64_t>(block->length, budget - retired));
        const a32::FastOp* const begin = block->fast.data();
        const a32::FastOp* const end = begin + count;
        const a32::FastOp* op = begin;
        while (op != end) {
            const a32::FastOp* const start = op;
            unsigned done = a32::fast_next;
            for (; op != end; ++op) {
                done = op->run(cpu, *op, memory);
                if (done != a32::fast_next) [[unlikely]] break;
            }
            retired += static_cast<std::uint64_t>(op - start);
            if (op == end) {
                cpu.pc = end[-1].next_key;
                cpu.a32_itstate = end[-1].next_it;
                break;
            }
            if (done == a32::fast_jump) { ++retired; break; }
            if (op != begin) {
                cpu.pc = op[-1].next_key;
                cpu.a32_itstate = op[-1].next_it;
            }
            if (done == a32::fast_fallback) {
                // Nothing changed. Runtime::step() executes the instruction or reports its event.
                ++stats_.fallbacks;
                auto result = runtime_.step();
                retired += result.retired;
                if (result.event != Event::retired) { result.retired = retired; return result; }
                if (cpu.pc != op->next_key) break;
                ++op;
                continue;
            }
            if (done != a32::fast_generic) [[unlikely]] {
                // An inline function returned (the write-back above did not apply: it is the first op of its
                // block), a fetch fault, or UDF #0, reported as Runtime::step() does with the PC at the instruction.
                if (done == fast_inline_return) break;
                if (done == fast_fetch_fault)
                    return {Event::memory_fault, retired, a32::key_pc(cpu.pc), 0, MemoryAccess::execute, false};
                return {Event::undefined_instruction, retired};
            }
            // fast_generic: an instruction without a fast handler.
            const Entry* const e = &block->entries[op->imm];
            ++stats_.generic;
            ++d.generic_forms[static_cast<std::size_t>(e->insn.form)];
            bool executed = false;
            if (e->route_vfp) {
                const auto vfp_done = vfp::execute_a32(cpu, memory, e->word, e->pc);
                if (vfp_done.kind == vfp::Execution::Kind::fault)
                    return {Event::memory_fault, retired, vfp_done.address, 0, vfp_done.access, vfp_done.alignment};
                if (vfp_done.kind == vfp::Execution::Kind::unsupported)
                    return {Event::unsupported_instruction, retired, e->pc, e->word, MemoryAccess::execute, false, vfp_done.reason};
                executed = vfp_done.kind == vfp::Execution::Kind::retired;
            }
            if (!executed) {
                const auto result = e->prepared.run ? e->prepared.run(cpu, e->insn, d.adapter, e->prepared.info)
                                                    : a32::execute_a32(cpu, e->insn, d.adapter);
                if (result.event != a32::StepEvent::retired) [[unlikely]] {
                    const Event event = map_event(result.event);
                    if (event == Event::supervisor_call) ++retired;
                    return {event, retired, result.fault_address, result.syndrome, result.access, result.alignment};
                }
            }
            ++retired;
            if (cpu.pc != e->next_key) break;
            ++op;
        }
        if (Single) return {Event::retired, retired};
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
Result BlockRunner::step_block(std::uint64_t budget) noexcept { return execute<true>(budget); }
Result BlockRunner::run(std::uint64_t budget) noexcept { return execute<false>(budget); }
}
