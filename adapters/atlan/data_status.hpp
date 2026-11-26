// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include <array>
#include <span>
#include <string>
#include <string_view>
#include <vector>

// What the launcher shows about an ATLAN folder before the app runs: which of its top folders are there, the app's
// own name and version, and the dates of the map and safe-driving data. Only small files and file tails are read.
namespace atlan::shell {
struct AtlanFolderCheck {
    std::string_view name;  // as the launcher shows it
    bool present{};
};

struct AtlanDataStatus {
    bool found{};                                // the ATLAN folder itself exists
    std::array<AtlanFolderCheck, 4> folders{{{"APP"}, {"UI"}, {"MAP"}, {"Sys"}}};
    bool package{};                              // an app package (atlan.dat or an APK) is in place
    std::string app_name, app_version;           // e.g. "Atlan5", "8.2.35"; empty when unknown
    std::string map, route, search;              // navigation data dates as YY.MM.DD; empty when unknown
    std::string safety_main, safety_sub;         // safe-driving data dates as YY.MM.DD; empty when unknown

    [[nodiscard]] bool folders_present() const noexcept;
    [[nodiscard]] bool ready() const noexcept { return found && folders_present() && package; }
    [[nodiscard]] std::string app_label() const;  // "Atlan5 8.2.35", or what is known of it
};

// Inspects the ATLAN folder at `folder` (folder and file names in any case).
[[nodiscard]] AtlanDataStatus inspect_atlan_data(const std::string& folder);

// Folders directly inside `parent` that look like an ATLAN folder (they hold at least one of APP, UI, MAP, Sys in
// any case), by name. Lets several data sets sit side by side, e.g. ATLAN and ATLAN_RTA in the app's Documents.
[[nodiscard]] std::vector<std::string> find_atlan_folders(const std::string& parent);

// The child of `picked` named ATLAN (any case) when there is one, e.g. when the user picked a whole SD card copy;
// otherwise `picked` itself.
[[nodiscard]] std::string resolve_atlan_folder(const std::string& picked);

// The date in a data file's version trailer as YY.MM.DD; empty when the file or its trailer is missing.
[[nodiscard]] std::string read_data_version(const std::string& path);

// SHA-256 of the native library in the ATLAN folder's app package (the one the session would run); empty when there
// is no readable package. The package is read and unpacked in full, so the result is kept per path, size and
// modification time and checking the same package again costs only finding it.
[[nodiscard]] std::string package_library_sha256(const std::string& folder);
}
