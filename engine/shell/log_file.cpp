// SPDX-License-Identifier: GPL-2.0-or-later
#include "shell/log_file.hpp"
#include <algorithm>
#include <ctime>
#include <filesystem>
#include <vector>

namespace atlan::shell {
SessionLogFile::~SessionLogFile() {
    if (file_) std::fclose(file_);
}

bool SessionLogFile::open(const std::string& directory, std::size_t keep) {
    const std::lock_guard lock(mutex_);
    if (file_) return true;
    std::error_code ec;
    std::filesystem::create_directories(directory, ec);
    std::vector<std::filesystem::path> earlier;
    for (const auto& entry : std::filesystem::directory_iterator(directory, ec)) {
        const auto name = entry.path().filename().string();
        if (name.starts_with("session-") && name.ends_with(".log")) earlier.push_back(entry.path());
    }
    std::sort(earlier.begin(), earlier.end());  // the timestamp in the name orders them
    for (std::size_t i = 0; i + (keep == 0 ? 0 : keep - 1) < earlier.size(); ++i) std::filesystem::remove(earlier[i], ec);
    const std::time_t now = std::time(nullptr);
    std::tm local{};
    localtime_r(&now, &local);
    char stamp[32];
    std::strftime(stamp, sizeof stamp, "%Y%m%d-%H%M%S", &local);
    path_ = (std::filesystem::path(directory) / (std::string("session-") + stamp + ".log")).string();
    file_ = std::fopen(path_.c_str(), "a");
    opened_ = std::chrono::steady_clock::now();
    return file_ != nullptr;
}

void SessionLogFile::write(std::string_view line) {
    const std::lock_guard lock(mutex_);
    if (!file_) return;
    const double seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - opened_).count();
    std::fprintf(file_, "%10.3f %.*s\n", seconds, static_cast<int>(line.size()), line.data());
    std::fflush(file_);
}
}
