// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include <chrono>
#include <cstdio>
#include <mutex>
#include <string>
#include <string_view>

namespace atlan::shell {
// A debugging aid: every line of one run of the app, written as it happens to DIRECTORY/session-YYYYMMDD-HHMMSS.log
// and flushed line by line, so the file is complete up to a crash or exit. Each line is prefixed with the seconds
// since the file was opened. Opening removes all but the newest `keep - 1` earlier session logs in the directory.
// Safe to call from several threads. A file that cannot be opened makes write() a no-op.
class SessionLogFile {
public:
    SessionLogFile() = default;
    ~SessionLogFile();
    SessionLogFile(const SessionLogFile&) = delete;
    SessionLogFile& operator=(const SessionLogFile&) = delete;
    // False if the directory or the file cannot be created.
    bool open(const std::string& directory, std::size_t keep = 5);
    void write(std::string_view line);
    [[nodiscard]] const std::string& path() const noexcept { return path_; }
private:
    std::mutex mutex_;
    std::FILE* file_{};
    std::string path_;
    std::chrono::steady_clock::time_point opened_{};
};
}
