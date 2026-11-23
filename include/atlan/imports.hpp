// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include "atlan/elf.hpp"
#include "atlan/runtime.hpp"
#include <array>
#include <functional>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>

namespace atlan {
class ImportDispatcher;

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
    [[nodiscard]] Runtime& runtime() noexcept { return runtime_; }
    [[nodiscard]] ImportDispatcher& dispatcher() noexcept { return dispatcher_; }
    [[nodiscard]] std::string_view name() const noexcept { return name_; }
private:
    ImportDispatcher& dispatcher_;
    Runtime& runtime_;
    std::string_view name_;
};

// A service returns std::nullopt on success, otherwise a diagnostic message.
// A failing service stops execution; it never fabricates a successful result.
using ImportService = std::function<std::optional<std::string>(ImportCall&)>;

enum class Stop : std::uint8_t {
    returned, budget_exhausted, cpu_event, unresolved_import, service_error
};
struct CallOutcome {
    Stop stop{Stop::returned};
    Result cpu{};
    std::uint64_t retired{};
    std::string import;   // unresolved import or failing service
    std::string message;  // service diagnostic
};

// Routes guest execution of unresolved import trap addresses to registered host
// services. Anything unregistered remains an explicit stop (Stop::unresolved_import).
class ImportDispatcher {
public:
    static constexpr unsigned max_depth = 16;
    // Registers a service for one symbol name; false if the name is already bound.
    bool bind(std::string name, ImportService service);
    // Binds one host-reserved, unmapped, 4-byte aligned trap address to a name.
    // False if the address is misaligned or already bound.
    bool bind_trap(std::uint32_t address, std::string name);
    // Maps an image's trap addresses to symbol names. Function imports only.
    void attach(const elf::Image& image);

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

private:
    [[nodiscard]] CallOutcome run_to_return(Runtime& runtime, std::uint32_t return_target);
    std::unordered_map<std::string, ImportService> services_;
    std::unordered_map<std::uint32_t, std::string> traps_;  // trap base address -> name
    std::uint32_t return_target_{};
    std::uint64_t remaining_{};
    unsigned depth_{};
};
}
