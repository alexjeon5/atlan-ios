// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include "atlan/imports.hpp"
#include <cstdint>
#include <cstdio>
#include <optional>
#include <span>
#include <filesystem>
#include <functional>
#include <string>
#include <unordered_map>
#include <unordered_set>
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
    // Fallback for an absolute guest path that no alias matched: the path up to and including its first component
    // equal to `component` (ASCII case-insensitive) is replaced by `host_prefix`. First matching anchor wins.
    void add_anchor(std::string component, std::string host_prefix);
    // Paths beneath the root (after aliases and anchors) whose leading components equal `host_prefix` (ASCII
    // case-insensitive) are served from `directory`, which may lie outside the root, e.g. a folder the user picked
    // elsewhere. First matching mount wins, so add the more specific prefix first.
    void add_mount(std::filesystem::path host_prefix, std::filesystem::path directory);
    // A file opened read-only through fopen whose path beneath the root equals `relative` (ASCII case-insensitive,
    // after aliases) is served as `transform(its bytes)` from an unnamed temporary file; the host file is untouched.
    void add_read_filter(std::string relative, std::function<std::string(std::string)> transform);
    // Host location of a guest path (aliases, anchors, case-insensitive lookup); empty if it escapes the root.
    [[nodiscard]] std::filesystem::path host_path(const std::string& guest_path) const;
    [[nodiscard]] std::size_t open_files() const noexcept;
    [[nodiscard]] std::size_t open_descriptors() const noexcept;
    // Size of the regular file behind a POSIX descriptor opened through open(), for fstat.
    [[nodiscard]] std::optional<std::uint64_t> descriptor_size(std::uint32_t fd) const;
    // The host descriptor (and whether it is writable) behind a readable guest POSIX descriptor, for file-backed mappings; nullopt for a bad one.
    // Still owned by this object: callers must not close it and must not use it after the guest closes the fd.
    [[nodiscard]] std::optional<std::pair<int, bool>> host_descriptor(std::uint32_t fd) const;  // (descriptor, opened writable)
    // Collects the host location of every guest path the services resolve (opens, stats, listings, removals) from
    // record_paths() until take_recorded_paths(), for a host that keeps a result derived from guest file reads.
    void record_paths() noexcept { recording_ = true; recorded_.clear(); }
    [[nodiscard]] std::vector<std::filesystem::path> take_recorded_paths() noexcept {
        recording_ = false;
        return std::move(recorded_);
    }
private:
    [[nodiscard]] std::string alias(const std::string& path) const;
    [[nodiscard]] std::filesystem::path locate(const std::filesystem::path& relative) const;
    [[nodiscard]] std::filesystem::path find(const std::filesystem::path& relative) const;
    bool recording_{};
    mutable std::vector<std::filesystem::path> recorded_;
    // Names in one host directory, read once for the case-insensitive lookup of locate(). Dropped whenever a
    // service creates, removes or renames something; a file another program adds is still found by its exact name.
    struct Listing { std::unordered_set<std::string> names; std::unordered_map<std::string, std::string> lower; };
    [[nodiscard]] const Listing& listing(const std::filesystem::path& directory) const;
    void changed() const noexcept { listings_.clear(); }
    mutable std::unordered_map<std::string, Listing> listings_;
    [[nodiscard]] std::FILE* open_filtered(const std::filesystem::path& relative) const;
    std::vector<std::pair<std::string, std::string>> aliases_;
    std::vector<std::pair<std::string, std::string>> anchors_;
    std::vector<std::pair<std::filesystem::path, std::filesystem::path>> mounts_;
    std::vector<std::pair<std::string, std::function<std::string(std::string)>>> read_filters_;
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
