// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include "atlan/imports.hpp"
#include <map>

namespace atlan {
// Virtual file descriptor table. Guest descriptors are indices into this table,
// never host descriptors; guest syscalls are not forwarded. Descriptors 0-2 are
// the standard streams. Opening files is added with the file/map milestone.
class VirtualFiles {
public:
    // errno_address is a guest word the services write error codes to.
    explicit VirtualFiles(std::uint32_t errno_address) : errno_address_(errno_address) {}
    void install(ImportDispatcher& dispatcher);
private:
    struct Descriptor { std::uint32_t status_flags{}; std::uint32_t descriptor_flags{}; };
    std::uint32_t errno_address_;
    std::map<std::uint32_t, Descriptor> descriptors_{{0, {0, 0}}, {1, {1, 0}}, {2, {1, 0}}};
};
}
