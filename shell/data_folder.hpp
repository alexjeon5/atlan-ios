// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include <cstdint>
#include <string>
#include <vector>

namespace atlan::shell {
struct DataFolderStatus {
    bool ok{};                         // readable, not empty, every file present on this device
    std::uint64_t files{};             // regular files found, including ones not downloaded
    std::uint64_t bytes{};             // logical size of the files that are present
    std::vector<std::string> pending;  // files a cloud provider keeps off the device (not downloaded)
    std::string message;               // empty when ok, otherwise the explicit rejection reason
};

// Walks a user-chosen data folder where it lies; nothing is copied, opened or read. A file counts as not downloaded
// when it is a cloud placeholder (".name.icloud") or a dataless file (SF_DATALESS on Apple hosts). A session must not
// start while any file is pending: reading one would block on a download in the middle of the run.
[[nodiscard]] DataFolderStatus inspect_data_folder(const std::string& path) noexcept;

// `picked` itself, or its child named "ATLAN" (any case) when the user picked the parent folder.
[[nodiscard]] std::string resolve_map_root(const std::string& picked) noexcept;
}
