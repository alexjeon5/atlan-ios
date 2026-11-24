// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include "atlan/imports.hpp"
#include <cstdint>
#include <cstdio>
#include <optional>
#include <span>
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
    static constexpr std::size_t max_open = 256, max_dirs = 16;
    // errno_address: guest word receiving error codes. trace (optional) sees
    // every open attempt: guest path, mode, success.
    GuestStdio(std::filesystem::path root, std::uint32_t errno_address,
               std::function<void(const std::string&, const std::string&, bool)> trace = {})
        : root_(std::move(root)), errno_address_(errno_address), trace_(std::move(trace)) {}
    ~GuestStdio();
    GuestStdio(const GuestStdio&) = delete;
    GuestStdio& operator=(const GuestStdio&) = delete;
    void install(ImportDispatcher& dispatcher);
    // Guest paths starting with `guest_prefix` (whole components) are redirected to `host_prefix` beneath the root,
    // e.g. the app's absolute Android paths onto folders the user can reach. First matching alias wins.
    void add_alias(std::string guest_prefix, std::string host_prefix);
    [[nodiscard]] std::size_t open_files() const noexcept;
    [[nodiscard]] std::size_t open_descriptors() const noexcept;
    // Size of the regular file behind a POSIX descriptor opened through open(), for fstat.
    [[nodiscard]] std::optional<std::uint64_t> descriptor_size(std::uint32_t fd) const;
    // The host descriptor behind a readable guest POSIX descriptor, for file-backed mappings; nullopt for a bad one.
    // Still owned by this object: callers must not close it and must not use it after the guest closes the fd.
    [[nodiscard]] std::optional<int> host_descriptor(std::uint32_t fd) const;
private:
    [[nodiscard]] std::string alias(const std::string& path) const;
    [[nodiscard]] std::filesystem::path locate(const std::filesystem::path& relative) const;
    std::vector<std::pair<std::string, std::string>> aliases_;
    std::filesystem::path root_;
    std::uint32_t errno_address_;
    std::function<void(const std::string&, const std::string&, bool)> trace_;
    struct Dir { std::vector<std::pair<std::string, bool>> entries; std::size_t next{}; bool open{}; };  // (name, is_directory)
    std::vector<Dir> dirs_;  // slot -> directory stream; handle = 0xe5000000 + 4*(slot+1)
    std::vector<std::FILE*> files_;
    struct Fd { int host{-1}; std::uint64_t offset{}; bool append{}; bool writable{}, readable{}; };
    std::vector<Fd> fds_;  // guest descriptor = 3 + index; host descriptors are never exposed  // slot -> host stream; handle = base + 4*(slot+1)
};
}
