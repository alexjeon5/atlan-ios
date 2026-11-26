// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include "atlan/imports.hpp"
#include <cstdint>
#include <functional>
#include <map>
#include <optional>

namespace atlan {
// Virtual file descriptor table. Guest descriptors are indices into this table,
// never host descriptors; guest syscalls are not forwarded. Descriptors 0-2 are
// the standard streams. Opening files is added with the file/map milestone.
class VirtualFiles {
public:
    // errno_address is a guest word the services write error codes to.
    explicit VirtualFiles(std::uint32_t errno_address) : errno_address_(errno_address) {}
    void install(ImportDispatcher& dispatcher);
    // Descriptors this table does not own (e.g. GuestStdio's open() files) report a regular
    // file of the returned size from fstat instead of EBADF.
    void set_regular_file_size(std::function<std::optional<std::uint64_t>(std::uint32_t)> size_of) { size_of_ = std::move(size_of); }
private:
    struct Descriptor { std::uint32_t status_flags{}; std::uint32_t descriptor_flags{}; };
    std::uint32_t errno_address_;
    std::function<std::optional<std::uint64_t>(std::uint32_t)> size_of_;
    std::map<std::uint32_t, Descriptor> descriptors_{{0, {0, 0}}, {1, {1, 0}}, {2, {1, 0}}};
};
}
