// SPDX-License-Identifier: GPL-2.0-or-later
#include "adapters/atlan/data_status.hpp"
#include "adapters/atlan/app_package.hpp"
#include <algorithm>
#include <filesystem>
#include <fstream>
#include <initializer_list>
#include <optional>

namespace atlan::shell {
namespace {
namespace fs = std::filesystem;

constexpr std::array<VerifiedApp, 2> verified{{{"Atlan5", "8.2.35"}, {"Atlan3DA", "6.4.21"}}};
constexpr std::string_view trailer_tag = "ATLANVERINFO";
constexpr std::size_t trailer_size = 48, trailer_tag_size = 32;

std::string lower(std::string_view text) {
    std::string result(text);
    for (auto& c : result) if (c >= 'A' && c <= 'Z') c = static_cast<char>(c + 32);
    return result;
}

std::optional<fs::path> child(const fs::path& folder, std::string_view name, bool directory) {
    std::error_code error;
    const auto wanted = lower(name);
    for (fs::directory_iterator it(folder, error), end; !error && it != end; it.increment(error)) {
        if (lower(it->path().filename().string()) != wanted) continue;
        if (directory ? it->is_directory(error) : it->is_regular_file(error)) return it->path();
    }
    return std::nullopt;
}

std::optional<fs::path> find(const fs::path& folder, std::initializer_list<std::string_view> parts) {
    fs::path current = folder;
    std::size_t left = parts.size();
    for (const auto part : parts) {
        const auto next = child(current, part, --left != 0);
        if (!next) return std::nullopt;
        current = *next;
    }
    return current;
}

std::string version_at(const fs::path& folder, std::initializer_list<std::string_view> parts) {
    const auto path = find(folder, parts);
    return path ? read_data_version(path->string()) : std::string{};
}

// "Name (1.2.3)" → name and version.
void read_app_info(const fs::path& app, AtlanDataStatus& status) {
    const auto path = child(app, "atlan3d.inf", false);
    if (!path) return;
    std::ifstream input(*path, std::ios::binary);
    std::string text(256, '\0');
    input.read(text.data(), static_cast<std::streamsize>(text.size()));
    text.resize(static_cast<std::size_t>(input.gcount()));
    const auto open = text.find('('), close = text.find(')', open == std::string::npos ? 0 : open);
    const auto trim = [](std::string value) {
        const auto first = value.find_first_not_of(" \t\r\n");
        if (first == std::string::npos) return std::string{};
        return value.substr(first, value.find_last_not_of(" \t\r\n") - first + 1);
    };
    if (open == std::string::npos || close == std::string::npos) { status.app_name = trim(text.substr(0, text.find('\0'))); return; }
    status.app_name = trim(text.substr(0, open));
    status.app_version = trim(text.substr(open + 1, close - open - 1));
}
}

bool AtlanDataStatus::folders_present() const noexcept {
    return std::all_of(folders.begin(), folders.end(), [](const auto& folder) { return folder.present; });
}

std::string AtlanDataStatus::app_label() const {
    if (app_name.empty() || app_version.empty()) return app_name + app_version;
    return app_name + " " + app_version;
}

std::string read_data_version(const std::string& path) {
    std::ifstream input(path, std::ios::binary);
    if (!input) return {};
    input.seekg(0, std::ios::end);
    const auto size = static_cast<std::streamoff>(input.tellg());
    if (size < static_cast<std::streamoff>(trailer_size)) return {};
    input.seekg(size - static_cast<std::streamoff>(trailer_size));
    std::array<char, trailer_size> tail{};
    if (!input.read(tail.data(), tail.size())) return {};
    const std::string_view tag(tail.data(), trailer_tag_size);
    if (tag.substr(0, trailer_tag.size()) != trailer_tag ||
        tag.substr(trailer_tag.size()).find_first_not_of('\0') != std::string_view::npos)
        return {};
    const std::string_view date(tail.data() + trailer_tag_size, 6);
    if (!std::all_of(date.begin(), date.end(), [](char c) { return c >= '0' && c <= '9'; })) return {};
    return std::string(date.substr(0, 2)) + "." + std::string(date.substr(2, 2)) + "." + std::string(date.substr(4, 2));
}

AtlanDataStatus inspect_atlan_data(const std::string& folder) {
    AtlanDataStatus status;
    std::error_code error;
    if (folder.empty() || !fs::is_directory(folder, error)) return status;
    status.found = true;
    for (auto& check : status.folders) check.present = child(folder, check.name, true).has_value();
    if (const auto app = child(folder, "APP", true)) read_app_info(*app, status);
    status.package = !find_package_in(folder).empty();
    status.map = version_at(folder, {"MAP", "MAP", "base3d.map"});
    status.route = version_at(folder, {"MAP", "NDB", "road.ndb"});
    status.search = version_at(folder, {"MAP", "SEARCH", "SCHETD", "nvr_etd.sdb"});
    status.safety_main = version_at(folder, {"MAP", "MAP", "camerat1.map"});
    status.safety_sub = version_at(folder, {"MAP", "MAP", "camerat2.map"});
    return status;
}

std::string resolve_atlan_folder(const std::string& picked) {
    if (picked.empty()) return picked;
    if (const auto atlan = child(picked, "ATLAN", true)) return atlan->string();
    return picked;
}

std::vector<std::string> find_atlan_folders(const std::string& parent) {
    std::vector<std::string> found;
    std::error_code error;
    if (parent.empty()) return found;
    for (fs::directory_iterator it(parent, error), end; !error && it != end; it.increment(error)) {
        if (!it->is_directory(error) || it->path().filename().string().starts_with(".")) continue;
        for (const auto name : {"APP", "UI", "MAP", "Sys"})
            if (child(it->path(), name, true)) { found.push_back(it->path().string()); break; }
    }
    std::sort(found.begin(), found.end());
    return found;
}

std::span<const VerifiedApp> verified_apps() noexcept { return verified; }

bool is_verified(const AtlanDataStatus& status) noexcept {
    return std::any_of(verified.begin(), verified.end(), [&](const VerifiedApp& app) {
        return app.name == status.app_name && app.version == status.app_version;
    });
}
}
