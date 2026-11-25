// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include "atlan/block_cache.hpp"
#include "atlan/elf.hpp"
#include "atlan/runtime.hpp"
#include <array>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>

namespace atlan {
class ImportDispatcher;
class GuestThreads;
class GuestProfiler;

// Arguments and result of one guest call into a host compatibility service.
// arg() reads r0-r3 only. Word arguments past the fourth live on the guest
// stack and are read with read_arg(), which can fail. Guest addresses are never
// host pointers.
class ImportCall {
public:
    ImportCall(ImportDispatcher& dispatcher, Runtime& runtime, std::string_view name) noexcept
        : dispatcher_(dispatcher), runtime_(runtime), name_(name) {}
    [[nodiscard]] std::uint32_t arg(unsigned index) const noexcept {
        return index < 4 ? static_cast<std::uint32_t>(runtime_.cpu.registers[index]) : 0;
    }
    // Word argument `index` of a call on entry: r0-r3, then [sp], [sp+4], ...
    [[nodiscard]] bool read_arg(unsigned index, std::uint32_t& out) noexcept {
        if (index < 4) { out = arg(index); return true; }
        std::array<std::byte, 4> bytes{};
        const std::uint64_t at = std::uint64_t{static_cast<std::uint32_t>(runtime_.cpu.registers[13])} + 4ULL * (index - 4);
        if (at + 4 > 0x100000000ULL || !runtime_.memory.read(static_cast<std::uint32_t>(at), bytes)) return false;
        out = 0;
        for (unsigned i = 0; i < 4; ++i) out |= std::to_integer<std::uint32_t>(bytes[i]) << (8 * i);
        return true;
    }
    void set_result(std::uint32_t value) noexcept { runtime_.cpu.registers[0] = value; }
    // Blocks the calling guest thread instead of returning. The service must not touch
    // any register first: the thread resumes by executing the same import again, so a
    // blocking service is a retry loop (check, block, check again). `key` names what
    // wakes the thread (GuestThreads::wake); `deadline_ns` (0: none) is an absolute
    // GuestThreads::now_ns() time after which the thread retries on its own.
    void block(std::uint32_t key, std::uint64_t deadline_ns = 0) noexcept {
        blocked_ = true; block_key_ = key; block_deadline_ = deadline_ns;
    }
    // The service already set the guest PC (for example thread exit); do not return to LR.
    void transfer() noexcept { transferred_ = true; }
    // Only for a service that replaces a guest function (ImportDispatcher::replace_function): run the guest's
    // own code for this call instead. The service must not have changed any guest state. Meant for rare inputs,
    // because each use drops the block cache.
    void decline() noexcept { declined_ = true; }
    [[nodiscard]] bool declined() const noexcept { return declined_; }
    [[nodiscard]] bool blocked() const noexcept { return blocked_; }
    [[nodiscard]] bool transferred() const noexcept { return transferred_; }
    [[nodiscard]] std::uint32_t block_key() const noexcept { return block_key_; }
    [[nodiscard]] std::uint64_t block_deadline() const noexcept { return block_deadline_; }
    [[nodiscard]] Runtime& runtime() noexcept { return runtime_; }
    [[nodiscard]] ImportDispatcher& dispatcher() noexcept { return dispatcher_; }
    [[nodiscard]] std::string_view name() const noexcept { return name_; }
private:
    ImportDispatcher& dispatcher_;
    Runtime& runtime_;
    std::string_view name_;
    bool blocked_{}, transferred_{}, declined_{};
    std::uint32_t block_key_{};
    std::uint64_t block_deadline_{};
};

// A service returns std::nullopt on success, otherwise a diagnostic message.
// A failing service stops execution; it never fabricates a successful result.
using ImportService = std::function<std::optional<std::string>(ImportCall&)>;

enum class Stop : std::uint8_t {
    returned, budget_exhausted, cpu_event, unresolved_import, service_error,
    deadlock  // every guest thread is blocked and none can be woken (message lists the waits)
};
struct CallOutcome {
    Stop stop{Stop::returned};
    Result cpu{};
    std::uint64_t retired{};
    std::uint32_t thread{};  // guest thread whose context is loaded in the Runtime when a stop is not `returned` (0: none)
    std::string import;   // unresolved import or failing service
    std::string message;  // service diagnostic
};

// Routes guest execution of unresolved import trap addresses to registered host
// services. Anything unregistered remains an explicit stop (Stop::unresolved_import).
class ImportDispatcher {
public:
    static constexpr unsigned max_depth = 16;
    // Instructions one guest callback may retire, independent of the budget left to its caller.
    static constexpr std::uint64_t callback_budget = 1ULL << 32;
    // Registers a service for one symbol name; false if the name is already bound.
    bool bind(std::string name, ImportService service);
    // Binds one host-reserved, unmapped, 4-byte aligned trap address to a name.
    // False if the address is misaligned or already bound.
    bool bind_trap(std::uint32_t address, std::string name);
    // The service bound to `name`, or nullptr; for a service that wraps an earlier one (see rebind).
    [[nodiscard]] const ImportService* service(const std::string& name) const {
        const auto found = services_.find(name);
        return found == services_.end() ? nullptr : &found->second;
    }
    // Binds `name`, replacing any service bound before.
    void rebind(std::string name, ImportService service) {
        resolved_.clear();
        services_.insert_or_assign(std::move(name), std::move(service));
    }
    // Maps an image's trap addresses to symbol names. Function imports only.
    void attach(const elf::Image& image);
    // Replaces the guest function at `address` (bit 0: Thumb) by a host service: its first instruction becomes a
    // permanently undefined one, and executing it runs the service with the function's arguments, then returns
    // to LR as for an import. The service may decline a call (ImportCall::decline), which runs the original
    // code. Calls are counted under `name` with the imports. False if the address is not executable guest code
    // or already replaced.
    bool replace_function(Runtime& runtime, std::uint32_t address, std::string name, ImportService service);
    [[nodiscard]] bool replaces(std::uint32_t address) const noexcept { return hooks_.count(address & ~1U) != 0; }

    // Scalar AAPCS32 call with import dispatch. `budget` bounds every instruction
    // retired, including nested guest callbacks made by services.
    [[nodiscard]] CallOutcome call(Runtime& runtime, std::uint32_t target,
                                   std::span<const std::uint32_t> arguments,
                                   std::uint32_t stack_top, std::uint32_t return_target,
                                   std::uint64_t budget);
    // Guest callback from inside a service: preserves the caller's CPU state,
    // runs on the caller's stack and spends the remaining shared budget.
    [[nodiscard]] CallOutcome call_back(Runtime& runtime, std::uint32_t target,
                                        std::span<const std::uint32_t> arguments);
    [[nodiscard]] std::uint64_t remaining_budget() const noexcept { return remaining_; }
    // Diagnostics: how often each import ran since the last reset (for telling a spin/wait loop from long work).
    [[nodiscard]] const std::unordered_map<std::string, std::uint64_t>& import_counts() const noexcept { return counts_; }
    void reset_import_counts() {
        counts_.clear();
        times_.clear();
        resolved_.clear();
        for (auto& [address, hook] : hooks_) hook.count = nullptr;
    }
    // Diagnostics: also measure the host time each import's service takes (two clock reads per call). A service
    // that calls back into the guest includes that guest code and its imports.
    void set_import_timing(bool enabled) noexcept { timing_ = enabled; }
    [[nodiscard]] const std::unordered_map<std::string, std::uint64_t>& import_nanoseconds() const noexcept { return times_; }

    // Attaches the guest thread scheduler (optional). Without one the dispatcher runs a
    // single guest thread and blocking services report a deadlock diagnostic instead.
    void set_threads(GuestThreads* threads) noexcept { threads_ = threads; }
    [[nodiscard]] unsigned depth() const noexcept { return depth_; }
    // Attaches a per-function profiler (optional, block cache only). While one is attached guest code runs one
    // block at a time, which is slower; results are unchanged.
    void set_profiler(GuestProfiler* profiler) noexcept { profiler_ = profiler; }
    // Guest code runs from the pre-decoded block cache (BlockRunner) by default; false selects Runtime::step()
    // for every instruction. Both give the same results; the switch exists for comparison and diagnosis.
    void set_block_cache(bool enabled) noexcept { block_cache_ = enabled; }
    [[nodiscard]] bool block_cache() const noexcept { return block_cache_; }
    // The block cache of the Runtime last executed (statistics), or nullptr before the first cached run.
    [[nodiscard]] const BlockRunner* block_runner() const noexcept { return runner_.get(); }
    // Runs ready guest threads other than the caller's thread until `budget` instructions
    // are retired or none can run (all blocked or finished). Returns `returned` when idle.
    // Only valid outside any guest call; the Runtime's CPU state is left unchanged.
    [[nodiscard]] CallOutcome pump(Runtime& runtime, std::uint64_t budget);

private:
    [[nodiscard]] CallOutcome run_to_return(Runtime& runtime, std::uint32_t return_target, bool pump = false);
    [[nodiscard]] BlockRunner& runner_for(Runtime& runtime);
    GuestThreads* threads_{};
    GuestProfiler* profiler_{};
    std::unique_ptr<BlockRunner> runner_;  // block cache of the Runtime last executed
    Runtime* runner_runtime_{};
    bool block_cache_{true};
    std::unordered_map<std::string, ImportService> services_;
    std::unordered_map<std::string, std::uint64_t> counts_;
    std::unordered_map<std::string, std::uint64_t> times_;  // nanoseconds per import while timing_
    bool timing_{};
    std::unordered_map<std::uint32_t, std::string> traps_;  // trap base address -> name
    // Per trap address: the service and counter its name resolves to, so a call costs one integer lookup.
    // Cleared whenever a binding or the counters change; elements of the maps above never move.
    struct Resolved {
        const std::string* name{};
        const ImportService* service{};
        std::uint64_t* count{};
        std::uint64_t* nanoseconds{};
    };
    std::unordered_map<std::uint32_t, Resolved> resolved_;
    struct Hook {
        std::string name;
        ImportService service;
        std::uint64_t* count{};   // into counts_, looked up again after a reset
        std::uint32_t original{};  // the instruction the undefined one replaced
        bool thumb{};
    };
    std::unordered_map<std::uint32_t, Hook> hooks_;  // function address without bit 0
    [[nodiscard]] static bool patch(Runtime& runtime, std::uint32_t address, std::uint32_t value, unsigned size);
    std::uint32_t return_target_{};
    std::uint64_t remaining_{};
    unsigned depth_{};
};
}
