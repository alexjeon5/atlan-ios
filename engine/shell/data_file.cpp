// SPDX-License-Identifier: GPL-2.0-or-later
#include "shell/data_file.hpp"
#include <array>
#include <cerrno>
#include <algorithm>
#include <cstring>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

namespace atlan::shell {
namespace {
bool read_at(int fd, std::uint64_t offset, std::byte* out, std::size_t size) noexcept {
    std::size_t done = 0;
    while (done < size) {
        const auto got = ::pread(fd, out + done, size - done, static_cast<off_t>(offset + done));
        if (got < 0 && errno == EINTR) continue;
        if (got <= 0) return false;
        done += static_cast<std::size_t>(got);
    }
    return true;
}
}

DataFileStatus inspect_data_file(const std::string& path, std::string_view magic,
                                 std::uint64_t min_size) noexcept {
    DataFileStatus status;
    const int fd = ::open(path.c_str(), O_RDONLY | O_CLOEXEC);
    if (fd < 0) { status.message = std::string("cannot open: ") + std::strerror(errno); return status; }
    struct stat info{};
    if (::fstat(fd, &info) != 0) status.message = "cannot stat file";
    else if (!S_ISREG(info.st_mode)) status.message = "not a regular file";
    else if (info.st_size < 0 || static_cast<std::uint64_t>(info.st_size) < std::max<std::uint64_t>(min_size, magic.size()))
        status.message = "file is smaller than the minimum size";
    else {
        status.size = static_cast<std::uint64_t>(info.st_size);
        std::array<std::byte, 4096> window{};
        const auto edge = std::min<std::uint64_t>(window.size(), status.size);
        if (!read_at(fd, 0, window.data(), edge)) status.message = "head of file is unreadable";
        else if (!magic.empty() && std::memcmp(window.data(), magic.data(), magic.size()) != 0)
            status.message = "file header does not match the expected format";
        else if (!read_at(fd, status.size - edge, window.data(), edge)) status.message = "tail of file is unreadable";
        else status.ok = true;
    }
    ::close(fd);
    return status;
}
}
