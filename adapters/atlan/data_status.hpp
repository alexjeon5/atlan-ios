// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include <array>
#include <span>
#include <string>
#include <string_view>

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

// The child of `picked` named ATLAN (any case) when there is one, e.g. when the user picked a whole SD card copy;
// otherwise `picked` itself.
[[nodiscard]] std::string resolve_atlan_folder(const std::string& picked);

// The date in a data file's version trailer as YY.MM.DD; empty when the file or its trailer is missing.
[[nodiscard]] std::string read_data_version(const std::string& path);

// App builds this launcher has been run with.
struct VerifiedApp {
    std::string_view name, version;
};
[[nodiscard]] std::span<const VerifiedApp> verified_apps() noexcept;
[[nodiscard]] bool is_verified(const AtlanDataStatus& status) noexcept;
}
