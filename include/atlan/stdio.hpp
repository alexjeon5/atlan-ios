// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include "atlan/imports.hpp"
#include <cstdio>
#include <filesystem>
#include <functional>
#include <vector>

namespace atlan {
// Buffered C stdio (fopen family) over a host directory. Guest paths are
// resolved only beneath `root` (".." components and escapes are refused); the
// guest never sees host paths or descriptors. FILE* values are opaque handles.
// A missing file is a normal fopen failure: NULL with ENOENT in guest errno.
class GuestStdio {
public:
    static constexpr std::size_t max_open = 256;
    // errno_address: guest word receiving error codes. trace (optional) sees
    // every open attempt: guest path, mode, success.
    GuestStdio(std::filesystem::path root, std::uint32_t errno_address,
               std::function<void(const std::string&, const std::string&, bool)> trace = {})
        : root_(std::move(root)), errno_address_(errno_address), trace_(std::move(trace)) {}
    ~GuestStdio();
    GuestStdio(const GuestStdio&) = delete;
    GuestStdio& operator=(const GuestStdio&) = delete;
    void install(ImportDispatcher& dispatcher);
    [[nodiscard]] std::size_t open_files() const noexcept;
private:
    std::filesystem::path root_;
    std::uint32_t errno_address_;
    std::function<void(const std::string&, const std::string&, bool)> trace_;
    std::vector<std::FILE*> files_;  // slot -> host stream; handle = base + 4*(slot+1)
};
}
