// SPDX-License-Identifier: GPL-2.0-or-later
#include "atlan/imports.hpp"
#include <stdexcept>

namespace atlan {
bool ImportDispatcher::bind(std::string name, ImportService service) {
    return services_.emplace(std::move(name), std::move(service)).second;
}
void ImportDispatcher::attach(const elf::Image& image) {
    for (const auto& import : image.trapped_imports)
        if (import.type != 1) traps_[import.address] = import.name;  // not STT_OBJECT
}
CallOutcome ImportDispatcher::call(Runtime& runtime, std::uint32_t target,
                                   std::span<const std::uint32_t> arguments,
                                   std::uint32_t stack_top, std::uint32_t return_target,
                                   std::uint64_t budget) {
    if (depth_ != 0) throw std::logic_error("use call_back from inside an import service");
    if (arguments.size() > 4 || stack_top % 8 != 0 || return_target % 4 != 0 ||
        (target & ~1U) == return_target ||
        runtime.memory.translate(return_target, MemoryAccess::read) != nullptr)
        throw std::invalid_argument("invalid scalar AAPCS32 call frame");
    for (std::size_t i = 0; i < 4; ++i)
        runtime.cpu.registers[i] = i < arguments.size() ? arguments[i] : 0;
    runtime.cpu.registers[13] = stack_top;
    runtime.cpu.registers[14] = return_target;
    runtime.start(target);
    return_target_ = return_target;
    remaining_ = budget;
    return run_to_return(runtime, return_target);
}
CallOutcome ImportDispatcher::call_back(Runtime& runtime, std::uint32_t target,
                                        std::span<const std::uint32_t> arguments) {
    if (depth_ == 0 || depth_ >= max_depth) throw std::logic_error("guest callback outside a bounded service");
    if (arguments.size() > 4 || (target & ~1U) == return_target_)
        throw std::invalid_argument("invalid scalar AAPCS32 callback");
    const CpuState saved = runtime.cpu;
    for (std::size_t i = 0; i < 4; ++i)
        runtime.cpu.registers[i] = i < arguments.size() ? arguments[i] : 0;
    // Skip the caller's current frame, keeping 8-byte alignment.
    runtime.cpu.registers[13] = (saved.registers[13] - 256) & ~std::uint64_t{7};
    runtime.cpu.registers[14] = return_target_;
    runtime.start(target);
    auto outcome = run_to_return(runtime, return_target_);
    const auto result = runtime.cpu.registers[0];
    runtime.cpu = saved;
    runtime.cpu.registers[0] = result;
    return outcome;
}
CallOutcome ImportDispatcher::run_to_return(Runtime& runtime, std::uint32_t return_target) {
    struct Depth { unsigned& value; explicit Depth(unsigned& v) : value(v) { ++value; } ~Depth() { --value; } } depth(depth_);
    CallOutcome outcome;
    const auto finish = [&](Stop stop, Result cpu) {
        outcome.stop = stop;
        outcome.cpu = cpu;
        return outcome;
    };
    while (true) {
        if (!runtime.thumb() && runtime.pc() == return_target)
            return finish(Stop::returned, {Event::function_return, outcome.retired});
        if (remaining_ == 0) return finish(Stop::budget_exhausted, {Event::budget_exhausted, outcome.retired});
        auto result = runtime.step();
        outcome.retired += result.retired;
        remaining_ -= result.retired < remaining_ ? result.retired : remaining_;
        if (result.event == Event::retired) continue;
        result.retired = outcome.retired;
        const auto trap = result.event == Event::memory_fault && result.access == MemoryAccess::execute
                              ? traps_.find(runtime.pc() & ~1U) : traps_.end();
        if (trap == traps_.end()) return finish(Stop::cpu_event, result);
        const auto service = services_.find(trap->second);
        if (service == services_.end()) {
            outcome.import = trap->second;
            return finish(Stop::unresolved_import, result);
        }
        const auto link = static_cast<std::uint32_t>(runtime.cpu.registers[14]);
        ImportCall call(*this, runtime, trap->second);
        if (auto error = service->second(call)) {
            outcome.import = trap->second;
            outcome.message = std::move(*error);
            return finish(Stop::service_error, result);
        }
        // Return to the caller; LR bit 0 selects Thumb, as for BX LR.
        runtime.start(link);
    }
}
}
