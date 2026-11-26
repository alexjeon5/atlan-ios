// SPDX-License-Identifier: GPL-2.0-or-later
// Prints the entry for adapters/atlan/verified_versions.json of the build in an ATLAN folder: the app's name and
// version from its own version file and the SHA-256 of the native library inside its package. Nothing else is read
// out of the package, so the entry can go into the public list as it is.
#include "adapters/atlan/data_status.hpp"
#include <iostream>

int main(int argc, char** argv) {
    if (argc != 2) {
        std::cerr << "usage: atlan-verified-entry ATLAN_FOLDER\n";
        return 2;
    }
    const auto folder = atlan::shell::resolve_atlan_folder(argv[1]);
    const auto status = atlan::shell::inspect_atlan_data(folder);
    const auto digest = atlan::shell::package_library_sha256(folder);
    if (digest.empty()) {
        std::cerr << "no readable app package in " << folder << '\n';
        return 1;
    }
    if (status.app_name.empty() || status.app_version.empty()) {
        std::cerr << "the app's name or version is unknown (APP/atlan3d.inf); fill them in by hand\n";
    }
    std::cout << "{\"name\": \"" << status.app_name << "\", \"version\": \"" << status.app_version << "\", \"sha256\": \"" << digest << "\"}\n";
    return 0;
}
