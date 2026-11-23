// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include "atlan/imports.hpp"
#include <optional>
#include <string>

namespace atlan {
// Reads variadic arguments of the 32-bit ARM AAPCS. Two sources exist:
//  * a direct variadic call: remaining word arguments are r0-r3, then the stack;
//  * a va_list: on this ABI a single guest pointer to the next stack-style slot.
// 64-bit values (long long, double) occupy an even-aligned slot pair. Every read
// can fail on unmapped guest memory; a failed reader stays failed.
class VarArgs {
public:
    [[nodiscard]] static VarArgs from_call(ImportCall& call, unsigned first_index) noexcept;
    [[nodiscard]] static VarArgs from_va_list(Memory& memory, std::uint32_t list_pointer) noexcept;
    [[nodiscard]] bool next32(std::uint32_t& out) noexcept;
    [[nodiscard]] bool next64(std::uint64_t& out) noexcept;
    [[nodiscard]] bool failed() const noexcept { return failed_; }
private:
    ImportCall* call_{};
    Memory* memory_{};
    std::uint32_t pointer_{};  // va_list mode: address of the next slot
    unsigned slot_{};          // call mode: next word argument index
    bool failed_{};
};

// Bounded read of a NUL-terminated guest string; false on fault or overlong text.
[[nodiscard]] bool read_guest_string(Memory& memory, std::uint32_t address, std::size_t max_length,
                                     std::string& out);

// printf-style formatting of a guest format string against guest arguments.
// Supported: flags "-+ #0", width/precision (digits or '*'), length hh h l ll z t j,
// conversions d i u x X o c s p e E f F g G and %%. Anything else, including %n, is
// reported as an error, never skipped. On success the text is appended to `out`.
[[nodiscard]] std::optional<std::string> format_guest_printf(Memory& memory, std::uint32_t format,
                                                             VarArgs& args, std::string& out);
}
