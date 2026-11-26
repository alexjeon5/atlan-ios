// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include <optional>
#include <string>
#include <string_view>
#include <vector>

// The list of app builds this runtime has been run with. It is not built in: the launcher fetches it at start
// (verified_list_url), keeps the last copy it got and works without one. A build is identified by the SHA-256 of
// its ARM32 native library, so a repackaged or patched library with the same version text is not taken for a
// verified one. The list holds names, versions and digests only: no download address and nothing from the binary.
namespace atlan::shell {
// Where the launcher fetches the list: the file in this repository's main branch. A private repository cannot be
// read this way without credentials; the list then needs a public location of its own.
inline constexpr std::string_view verified_list_url =
    "https://raw.githubusercontent.com/alexjeon5/atlan-ios/main/adapters/atlan/verified_versions.json";

struct VerifiedBuild {
    std::string name, version;  // as the app's own version file states them, e.g. "Atlan5", "8.2.35"
    std::string sha256;         // of the native library inside the package, 64 lowercase hex digits
};

struct VerifiedList {
    std::string updated;  // date of the list, YYYY-MM-DD
    std::vector<VerifiedBuild> builds;
    // The build whose library has this digest, or nullptr.
    [[nodiscard]] const VerifiedBuild* find(std::string_view sha256) const noexcept;
};

// Reads a list document (format 1):
//   {"format": 1, "updated": "YYYY-MM-DD", "builds": [{"name": "...", "version": "...", "sha256": "..."}]}
// Only these keys are accepted, so nothing else (an address, a note) can slip into the public file. A newer format
// or anything malformed gives nullopt and `why`.
[[nodiscard]] std::optional<VerifiedList> parse_verified_list(std::string_view text, std::string& why);
}
