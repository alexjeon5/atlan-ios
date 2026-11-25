// SPDX-License-Identifier: GPL-2.0-or-later
#include "shell/data_folder.hpp"
#include <exception>
#include <filesystem>
#include <string_view>
#include <sys/stat.h>

namespace atlan::shell {
namespace {
bool is_placeholder(std::string_view name) {
    constexpr std::string_view suffix = ".icloud";
    return name.size() > suffix.size() + 1 && name.front() == '.' && name.substr(name.size() - suffix.size()) == suffix;
}
}

DataFolderStatus inspect_data_folder(const std::string& path) noexcept {
    DataFolderStatus status;
    try {
        std::error_code ec;
        if (!std::filesystem::is_directory(path, ec)) { status.message = "folder is missing"; return status; }
        std::filesystem::recursive_directory_iterator walk(path, std::filesystem::directory_options::none, ec);
        if (ec) { status.message = "folder is unreadable"; return status; }
        for (const std::filesystem::recursive_directory_iterator end; walk != end; walk.increment(ec)) {
            if (ec) { status.message = "folder is unreadable: " + ec.message(); return status; }
            const auto& entry = walk->path();
            struct stat info{};
            if (::lstat(entry.c_str(), &info) != 0) { status.message = "cannot stat " + entry.filename().string(); return status; }
            if (!S_ISREG(info.st_mode)) continue;
            bool pending = is_placeholder(entry.filename().string());
#ifdef SF_DATALESS
            pending = pending || (info.st_flags & SF_DATALESS) != 0;
#endif
            ++status.files;
            if (pending) status.pending.push_back(entry.string());
            else status.bytes += static_cast<std::uint64_t>(info.st_size);
        }
        if (ec) status.message = "folder is unreadable: " + ec.message();
        else if (status.files == 0) status.message = "folder is empty";
        else if (!status.pending.empty()) status.message = std::to_string(status.pending.size()) + " files are not downloaded";
        else status.ok = true;
    } catch (const std::exception& error) {
        status.ok = false;
        status.message = error.what();
    }
    return status;
}

std::string resolve_map_root(const std::string& picked) noexcept {
    try {
        std::error_code ec;
        for (const auto& entry : std::filesystem::directory_iterator(picked, ec)) {
            auto name = entry.path().filename().string();
            for (auto& c : name) if (c >= 'a' && c <= 'z') c = static_cast<char>(c - 32);
            if (name == "ATLAN" && entry.is_directory(ec)) return entry.path().string();
        }
    } catch (const std::exception&) {}
    return picked;
}
}
