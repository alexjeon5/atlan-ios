// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include "atlan/imports.hpp"
#include <optional>
#include <string>

namespace atlan {
class VarArgs;
// scanf-style parsing of `input` against a guest format string, storing through guest pointers taken from `args`
// (32-bit long, 8-byte double and long double, as ARM EABI). Supported: whitespace and literal matching, %%,
// assignment suppression, widths, length hh h l ll L j z t q, conversions d i u o x X p f e g E G a s c [set] n.
// Returns the number of assignments, or -1 when the input ends before the first conversion; an error message for
// an unsupported format or an unwritable pointer.
[[nodiscard]] std::optional<std::string> scan_guest(Memory& memory, const std::string& input, const std::string& format,
                                                    VarArgs& args, int& assigned);
// sscanf over guest memory.
void install_scanf_services(ImportDispatcher& dispatcher);
}
