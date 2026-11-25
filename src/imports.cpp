// SPDX-License-Identifier: GPL-2.0-or-later
#include "atlan/imports.hpp"
#include "atlan/profiler.hpp"
#include "atlan/threads.hpp"
#include <array>
#include <stdexcept>

namespace atlan {
bool ImportDispatcher::bind(std::string name, ImportService service) {
    resolved_.clear();
    return services_.emplace(std::move(name), std::move(service)).second;
}
bool ImportDispatcher::bind_trap(std::uint32_t address, std::string name) {
    if (address % 4 != 0) return false;
    resolved_.clear();
    return traps_.emplace(address, std::move(name)).second;
}
void ImportDispatcher::attach(const elf::Image& image) {
    resolved_.clear();
    for (const auto& import : image.trapped_imports)
        if (import.type != 1) traps_[import.address] = import.name;  // not STT_OBJECT
}
namespace {
constexpr std::uint32_t undefined_arm = 0xe7f000f0;  // UDF #0
constexpr std::uint32_t undefined_thumb = 0xde00;    // UDF #0
}
bool ImportDispatcher::patch(Runtime& runtime, std::uint32_t address, std::uint32_t value, unsigned size) {
    // Code pages are never writable for the guest; the host rewrites one through a short permission change,
    // which also invalidates every cache of decoded code.
    const std::uint32_t page = address & ~(Memory::page_size - 1U);
    std::array<std::byte, 4> bytes{};
    for (unsigned i = 0; i < size; ++i) bytes[i] = std::byte((value >> (8 * i)) & 255U);
    runtime.memory.protect(page, Memory::page_size, Permissions::read_write);
    const bool written = runtime.memory.write(address, std::span<const std::byte>(bytes.data(), size));
    runtime.memory.protect(page, Memory::page_size, Permissions::read_execute);
    return written;
}
bool ImportDispatcher::replace_function(Runtime& runtime, std::uint32_t address, std::string name, ImportService service) {
    const bool thumb = (address & 1U) != 0;
    address &= ~1U;
    const unsigned size = thumb ? 2 : 4;
    if (address % size != 0 || hooks_.count(address) != 0) return false;
    const std::byte* host = runtime.memory.translate(address, MemoryAccess::execute);
    if (host == nullptr) return false;
    Hook hook{std::move(name), std::move(service), nullptr, 0, thumb};
    for (unsigned i = 0; i < size; ++i) hook.original |= std::to_integer<std::uint32_t>(host[i]) << (8 * i);
    if (!patch(runtime, address, thumb ? undefined_thumb : undefined_arm, size)) return false;
    hooks_.emplace(address, std::move(hook));
    return true;
}
CallOutcome ImportDispatcher::call(Runtime& runtime, std::uint32_t target,
                                   std::span<const std::uint32_t> arguments,
                                   std::uint32_t stack_top, std::uint32_t return_target,
                                   std::uint64_t budget) {
    if (depth_ != 0) throw std::logic_error("use call_back from inside an import service");
    if (arguments.size() > Runtime::max_call_arguments || stack_top % 8 != 0 || return_target % 4 != 0 ||
        (target & ~1U) == return_target ||
        runtime.memory.translate(return_target, MemoryAccess::read) != nullptr)
        throw std::invalid_argument("invalid scalar AAPCS32 call frame");
    if (!runtime.load_arguments(arguments, stack_top)) {
        CallOutcome outcome;
        outcome.stop = Stop::cpu_event;
        outcome.cpu = {Event::memory_fault, 0, stack_top, 0, MemoryAccess::write};
        return outcome;
    }
    runtime.cpu.registers[14] = return_target;
    runtime.start(target);
    return_target_ = return_target;
    remaining_ = budget;
    return run_to_return(runtime, return_target);
}
CallOutcome ImportDispatcher::call_back(Runtime& runtime, std::uint32_t target,
                                        std::span<const std::uint32_t> arguments) {
    if (depth_ == 0 || depth_ >= max_depth) throw std::logic_error("guest callback outside a bounded service");
    if (arguments.size() > Runtime::max_call_arguments || (target & ~1U) == return_target_)
        throw std::invalid_argument("invalid scalar AAPCS32 callback");
    const CpuState saved = runtime.cpu;
    // Skip the caller's current frame, keeping 8-byte alignment.
    const auto frame = static_cast<std::uint32_t>((saved.registers[13] - 256) & ~std::uint64_t{7});
    if (!runtime.load_arguments(arguments, frame)) {
        CallOutcome outcome;
        outcome.stop = Stop::cpu_event;
        outcome.cpu = {Event::memory_fault, 0, frame, 0, MemoryAccess::write};
        runtime.cpu = saved;
        return outcome;
    }
    runtime.cpu.registers[14] = return_target_;
    runtime.start(target);
    auto outcome = run_to_return(runtime, return_target_);
    const auto result = runtime.cpu.registers[0];
    runtime.cpu = saved;
    runtime.cpu.registers[0] = result;
    return outcome;
}
CallOutcome ImportDispatcher::pump(Runtime& runtime, std::uint64_t budget) {
    if (depth_ != 0) throw std::logic_error("pump is only valid outside a guest call");
    if (threads_ == nullptr) return {};
    return_target_ = threads_->callback_return();
    remaining_ = budget;
    return run_to_return(runtime, return_target_, true);
}
BlockRunner& ImportDispatcher::runner_for(Runtime& runtime) {
    // A Runtime that reuses the address of a destroyed one is told apart by Memory::code_generation().
    if (!runner_ || runner_runtime_ != &runtime) {
        runner_ = std::make_unique<BlockRunner>(runtime);
        runner_runtime_ = &runtime;
    }
    return *runner_;
}
CallOutcome ImportDispatcher::run_to_return(Runtime& runtime, std::uint32_t return_target, bool pump) {
    struct Depth { unsigned& value; explicit Depth(unsigned& v) : value(v) { ++value; } ~Depth() { --value; } } depth(depth_);
    // Guest threads are multiplexed only by the outermost loop; a guest callback made by a
    // host service stays on its thread and can neither be preempted nor block.
    const bool threaded = threads_ != nullptr && depth_ == 1;
    CallOutcome outcome;
    BlockRunner* const runner = block_cache_ ? &runner_for(runtime) : nullptr;
    const auto finish = [&](Stop stop, Result cpu) {
        outcome.stop = stop;
        outcome.cpu = cpu;
        if (profiler_ != nullptr && depth_ == 1) {
            // A finished top-level call leaves its root frame open: the return to the sentinel is not a block.
            if (stop == Stop::returned && !pump) profiler_->unwind(threads_ != nullptr ? threads_->current_id() : 0);
            profiler_->pause();
        }
        if (threaded) {
            outcome.thread = threads_->current_id();
            threads_->end_run(runtime, stop == Stop::returned || stop == Stop::budget_exhausted);
        }
        return outcome;
    };
    if (threaded) threads_->begin_run(runtime, pump);
    bool entered = !pump;  // the host started a call on this thread; the profiler cannot tell from the registers
    std::uint64_t slice = 0;
    // Whether the checks at the top of the loop act on the current PC (a return or a thread exit).
    const auto stops_here = [&] {
        if (runtime.thumb()) return false;
        if (!threaded) return runtime.pc() == return_target;
        return threads_->current_is_main() ? !pump && runtime.pc() == return_target
                                           : runtime.pc() == threads_->exit_sentinel();
    };
    while (true) {
        if (threaded) {
            if (!threads_->has_current()) {
                const auto picked = threads_->schedule(runtime, !pump);
                if (picked == GuestThreads::Pick::idle) return finish(Stop::returned, {Event::function_return, outcome.retired});
                if (picked == GuestThreads::Pick::deadlock) {
                    outcome.message = threads_->deadlock_message();
                    return finish(Stop::deadlock, {Event::budget_exhausted, outcome.retired});
                }
                slice = 0;
            }
            if (!runtime.thumb() && !threads_->current_is_main() && runtime.pc() == threads_->exit_sentinel()) {
                threads_->thread_returned(runtime);
                slice = 0;
                continue;
            }
            if (!pump && threads_->current_is_main() && !runtime.thumb() && runtime.pc() == return_target)
                return finish(Stop::returned, {Event::function_return, outcome.retired});
            if (slice >= threads_->quantum()) {
                threads_->yield_current(runtime);
                slice = 0;
            }
        } else if (!runtime.thumb() && runtime.pc() == return_target) {
            return finish(Stop::returned, {Event::function_return, outcome.retired});
        }
        if (remaining_ == 0) return finish(Stop::budget_exhausted, {Event::budget_exhausted, outcome.retired});
        // The runner executes whole chains of blocks. It cannot pass the PCs the checks above look for, because
        // they are not executable: reaching one ends the run with an execute fault, and the loop goes back to
        // those checks instead of treating the PC as an import trap. The budget keeps the remaining count and
        // the thread quantum exact.
        Result result;
        if (runner != nullptr) {
            std::uint64_t limit = remaining_;
            if (threaded && threads_->quantum() - slice < limit) limit = threads_->quantum() - slice;
            const bool stops_fault = runtime.memory.translate(return_target, MemoryAccess::execute) == nullptr &&
                (!threaded || runtime.memory.translate(threads_->exit_sentinel(), MemoryAccess::execute) == nullptr);
            if (profiler_ != nullptr) [[unlikely]] {
                const std::uint32_t id = threads_ != nullptr ? threads_->current_id() : 0;
                const bool enters = entered && (!threaded || threads_->current_is_main());
                if (enters) {
                    entered = false;
                    if (depth_ == 1) profiler_->unwind(id);
                }
                profiler_->begin(runtime, id, enters);
                result = runner->step_block(limit);
                profiler_->end(runtime, result.retired);
            } else {
                result = stops_fault ? runner->run(limit) : runner->step_block(limit);
            }
            if (result.event == Event::budget_exhausted) result.event = Event::retired;
            if (stops_fault && result.event == Event::memory_fault && result.access == MemoryAccess::execute && stops_here()) {
                outcome.retired += result.retired;
                remaining_ -= result.retired < remaining_ ? result.retired : remaining_;
                slice += result.retired;
                continue;
            }
        } else {
            result = runtime.step();
        }
        outcome.retired += result.retired;
        remaining_ -= result.retired < remaining_ ? result.retired : remaining_;
        slice += result.retired;
        if (result.event == Event::retired) continue;
        result.retired = outcome.retired;
        const std::uint32_t trap_address = runtime.pc() & ~1U;
        Resolved target;
        Hook* hook = nullptr;
        if (result.event == Event::undefined_instruction) {
            // The undefined instruction a replaced guest function starts with.
            const auto found = hooks_.find(trap_address);
            if (found == hooks_.end() || found->second.thumb != runtime.thumb()) return finish(Stop::cpu_event, result);
            hook = &found->second;
            if (hook->count == nullptr) hook->count = &counts_[hook->name];
            target = {&hook->name, &hook->service, hook->count};
        } else if (result.event != Event::memory_fault || result.access != MemoryAccess::execute) {
            return finish(Stop::cpu_event, result);
        } else if (const auto known = resolved_.find(trap_address); known != resolved_.end()) {
            target = known->second;
        } else {
            const auto trap = traps_.find(trap_address);
            if (trap == traps_.end()) return finish(Stop::cpu_event, result);
            const auto service = services_.find(trap->second);
            if (service == services_.end()) {
                outcome.import = trap->second;
                return finish(Stop::unresolved_import, result);
            }
            target = {&trap->second, &service->second, &counts_[trap->second]};
            resolved_.emplace(trap_address, target);
        }
        const std::string& name = *target.name;
        const auto link = static_cast<std::uint32_t>(runtime.cpu.registers[14]);
        ++*target.count;
        ImportCall call(*this, runtime, name);
        if (auto error = (*target.service)(call)) {
            outcome.import = name;
            outcome.message = std::move(*error);
            return finish(Stop::service_error, result);
        }
        if (call.declined()) {
            if (hook == nullptr) {
                outcome.import = name;
                outcome.message = name + " declined a call, which only a replaced guest function can";
                return finish(Stop::service_error, result);
            }
            // Run the instruction the hook displaced in place, then put the hook back.
            const unsigned size = hook->thumb ? 2 : 4;
            --*target.count;
            ++counts_[name + " (declined)"];
            (void)patch(runtime, trap_address, hook->original, size);
            Result step = runtime.step();
            (void)patch(runtime, trap_address, hook->thumb ? undefined_thumb : undefined_arm, size);
            outcome.retired += step.retired;
            remaining_ -= step.retired < remaining_ ? step.retired : remaining_;
            slice += step.retired;
            if (step.event == Event::retired) continue;
            step.retired = outcome.retired;
            return finish(Stop::cpu_event, step);
        }
        if (call.blocked()) {
            if (!threaded) {
                outcome.import = name;
                outcome.message = name + " would block, which needs an attached guest thread scheduler and no host callback in between";
                return finish(Stop::service_error, result);
            }
            const auto picked = threads_->block_current(runtime, call.block_key(), call.block_deadline(), name, !pump);
            slice = 0;
            if (picked == GuestThreads::Pick::idle) return finish(Stop::returned, {Event::function_return, outcome.retired});
            if (picked == GuestThreads::Pick::deadlock) {
                outcome.import = name;
                outcome.message = threads_->deadlock_message();
                return finish(Stop::deadlock, result);
            }
            continue;
        }
        if (call.transferred()) continue;
        // Return to the caller; LR bit 0 selects Thumb, as for BX LR.
        runtime.start(link);
    }
}
}
