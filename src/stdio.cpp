// SPDX-License-Identifier: GPL-2.0-or-later
#include "atlan/stdio.hpp"
#include "atlan/abi.hpp"
#include <array>
#include <algorithm>
#include <cerrno>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>
#include <memory>

namespace atlan {
namespace {
constexpr std::uint32_t file_base = 0xe4000000;
constexpr std::uint32_t guest_enoent = 2, guest_eacces = 13, guest_einval = 22, guest_emfile = 24;
constexpr std::uint32_t dir_base = 0xe5000000, dirent_base = 0xe5100000, dirent_stride = 512;
constexpr std::uint32_t max_io = 16U * 1024 * 1024;
}
GuestStdio::~GuestStdio() {
    for (auto& fd : fds_) if (fd.host >= 0) ::close(fd.host);
    for (auto* file : files_) if (file) std::fclose(file);
}
std::size_t GuestStdio::open_files() const noexcept {
    std::size_t count = 0;
    for (auto* file : files_) count += file != nullptr;
    return count;
}
std::size_t GuestStdio::open_descriptors() const noexcept {
    std::size_t count = 0;
    for (const auto& fd : fds_) count += fd.host >= 0;
    return count;
}
std::optional<std::uint64_t> GuestStdio::descriptor_size(std::uint32_t fd) const {
    if (fd < 3 || fd - 3 >= fds_.size() || fds_[fd - 3].host < 0) return std::nullopt;
    struct ::stat st{};
    if (::fstat(fds_[fd - 3].host, &st) != 0) return std::nullopt;
    return static_cast<std::uint64_t>(st.st_size);
}
void GuestStdio::add_alias(std::string guest_prefix, std::string host_prefix) {
    while (guest_prefix.size() > 1 && guest_prefix.back() == '/') guest_prefix.pop_back();
    aliases_.emplace_back(std::move(guest_prefix), std::move(host_prefix));
}
std::string GuestStdio::alias(const std::string& path) const {
    for (const auto& [guest, host] : aliases_)
        if (path.size() >= guest.size() && path.compare(0, guest.size(), guest) == 0 &&
            (path.size() == guest.size() || path[guest.size()] == '/'))
            return host + path.substr(guest.size());
    return path;
}
void GuestStdio::install(ImportDispatcher& dispatcher) {
    const auto set_errno = [this](ImportCall& call, std::uint32_t code) {
        std::array<std::byte, 4> bytes{};
        for (unsigned i = 0; i < 4; ++i) bytes[i] = std::byte((code >> (8 * i)) & 255);
        return call.runtime().memory.write(errno_address_, bytes);
    };
    const auto stream = [this](std::uint32_t handle) -> std::FILE* {
        if (handle < file_base + 4 || (handle - file_base) % 4 != 0 || (handle - file_base) / 4 > files_.size()) return nullptr;
        return files_[(handle - file_base) / 4 - 1];
    };
    // int stat(const char* path, struct stat* out) / lstat: ARM Bionic stat64 layout (104 bytes),
    // regular files and directories beneath the root only.
    for (const char* name : {"stat", "lstat"}) {
        dispatcher.bind(name, [this, set_errno, name](ImportCall& call) -> std::optional<std::string> {
            std::string path;
            if (!read_guest_string(call.runtime().memory, call.arg(0), 1024, path) || path.empty())
                return std::string(name) + " path is unreadable, empty or too long";
            std::filesystem::path relative;
            bool escapes = false;
            for (const auto& part : std::filesystem::path(alias(path)).relative_path()) {
                if (part == "..") escapes = true;
                else if (part != ".") relative /= part;
            }
            std::error_code ec;
            const auto host = root_ / relative;
            const bool is_dir = !escapes && std::filesystem::is_directory(host, ec);
            const bool is_file = !escapes && !is_dir && std::filesystem::is_regular_file(host, ec);
            if (!is_dir && !is_file) {
                if (!set_errno(call, escapes ? guest_eacces : guest_enoent)) return std::string(name) + " could not write errno";
                call.set_result(0xffffffffU);
                return std::nullopt;
            }
            const std::uint64_t size = is_file ? std::filesystem::file_size(host, ec) : 4096;
            std::array<std::byte, 104> stat{};
            const auto put = [&](unsigned at, std::uint64_t value, unsigned bytes) {
                for (unsigned i = 0; i < bytes; ++i) stat[at + i] = std::byte((value >> (8 * i)) & 255);
            };
            put(16, is_dir ? 040755 : 0100644, 4);
            put(20, 1, 4);
            put(48, size, 8);
            put(56, 4096, 4);
            put(64, (size + 511) / 512, 8);
            if (!call.runtime().memory.write(call.arg(1), stat)) return std::string(name) + " guest memory fault";
            call.set_result(0);
            return std::nullopt;
        });
    }
    // Resolves a guest path beneath the root; false if it escapes through "..".
    const auto resolve = [this](const std::string& path, std::filesystem::path& host) {
        std::filesystem::path relative;
        for (const auto& part : std::filesystem::path(alias(path)).relative_path()) {
            if (part == "..") return false;
            if (part != ".") relative /= part;
        }
        host = root_ / relative;
        return !relative.empty();
    };
    // int remove(const char* path): a file or an empty directory beneath the root.
    dispatcher.bind("remove", [this, set_errno, resolve](ImportCall& call) -> std::optional<std::string> {
        std::string path;
        std::filesystem::path host;
        if (!read_guest_string(call.runtime().memory, call.arg(0), 1024, path) || path.empty())
            return "remove path is unreadable, empty or too long";
        std::error_code ec;
        std::uint32_t code = 0;
        if (!resolve(path, host)) code = guest_eacces;
        else if (!std::filesystem::exists(host, ec)) code = guest_enoent;
        else if (!std::filesystem::remove(host, ec)) code = guest_eacces;
        if (trace_) trace_(path, "remove", code == 0);
        if (code != 0 && !set_errno(call, code)) return "remove could not write errno";
        call.set_result(code == 0 ? 0 : 0xffffffffU);
        return std::nullopt;
    });
    // int rename(const char* from, const char* to): both beneath the root; replaces an existing file.
    dispatcher.bind("rename", [this, set_errno, resolve](ImportCall& call) -> std::optional<std::string> {
        std::string from, to;
        std::filesystem::path host_from, host_to;
        if (!read_guest_string(call.runtime().memory, call.arg(0), 1024, from) || from.empty() ||
            !read_guest_string(call.runtime().memory, call.arg(1), 1024, to) || to.empty())
            return "rename path is unreadable, empty or too long";
        std::error_code ec;
        std::uint32_t code = 0;
        if (!resolve(from, host_from) || !resolve(to, host_to)) code = guest_eacces;
        else if (!std::filesystem::exists(host_from, ec)) code = guest_enoent;
        else {
            std::filesystem::rename(host_from, host_to, ec);
            if (ec) code = guest_eacces;
        }
        if (trace_) trace_(from + " -> " + to, "rename", code == 0);
        if (code != 0 && !set_errno(call, code)) return "rename could not write errno";
        call.set_result(code == 0 ? 0 : 0xffffffffU);
        return std::nullopt;
    });
    // POSIX descriptors beneath the root. Guest fds start at 3; host descriptors stay private.
    // Flags are ARM Linux values: O_WRONLY 1, O_RDWR 2, O_CREAT 0100, O_TRUNC 01000, O_APPEND 02000.
    const auto fd_slot = [this](std::uint32_t fd) -> Fd* {
        if (fd < 3 || fd - 3 >= fds_.size() || fds_[fd - 3].host < 0) return nullptr;
        return &fds_[fd - 3];
    };
    const auto fail = [set_errno](ImportCall& call, std::uint32_t code, const char* what) -> std::optional<std::string> {
        if (!set_errno(call, code)) return std::string(what) + " could not write errno";
        call.set_result(0xffffffffU);
        return std::nullopt;
    };
    // int open(const char* path, int flags, ...mode)
    dispatcher.bind("open", [this, resolve, fail](ImportCall& call) -> std::optional<std::string> {
        std::string path;
        std::filesystem::path host;
        if (!read_guest_string(call.runtime().memory, call.arg(0), 1024, path) || path.empty())
            return "open path is unreadable, empty or too long";
        const auto flags = call.arg(1);
        const auto access = flags & 3;
        if (access == 3 || (flags & ~std::uint32_t{03 | 0100 | 0200 | 01000 | 02000 | 02000000}) != 0)
            return "open flags 0x" + std::to_string(flags) + " are not supported";
        if (!resolve(path, host)) { if (trace_) trace_(path, "open", false); return fail(call, guest_eacces, "open"); }
        int host_flags = access == 0 ? O_RDONLY : access == 1 ? O_WRONLY : O_RDWR;
        if (flags & 0100) host_flags |= O_CREAT;
        if (flags & 0200) host_flags |= O_EXCL;
        if (flags & 01000) host_flags |= O_TRUNC;
        std::size_t slot = 0;
        while (slot < fds_.size() && fds_[slot].host >= 0) ++slot;
        if (slot >= max_open) return fail(call, guest_emfile, "open");
        const int fd = ::open(host.c_str(), host_flags, 0644);
        if (trace_) trace_(path, "open flags=" + std::to_string(flags), fd >= 0);
        if (fd < 0) return fail(call, errno == ENOENT ? guest_enoent : errno == EEXIST ? 17 : errno == EISDIR ? 21 : guest_eacces, "open");
        if (fds_.size() <= slot) fds_.resize(slot + 1);
        fds_[slot] = {fd, 0, (flags & 02000) != 0, access != 0, access != 1};
        call.set_result(3 + static_cast<std::uint32_t>(slot));
        return std::nullopt;
    });
    // int close(int fd)
    dispatcher.bind("close", [fd_slot, fail](ImportCall& call) -> std::optional<std::string> {
        auto* fd = fd_slot(call.arg(0));
        if (!fd) return fail(call, 9, "close");
        ::close(fd->host);
        *fd = Fd{};
        call.set_result(0);
        return std::nullopt;
    });
    // ssize_t read(int fd, void* buf, size_t count)
    dispatcher.bind("read", [fd_slot, fail](ImportCall& call) -> std::optional<std::string> {
        auto* fd = fd_slot(call.arg(0));
        if (!fd || !fd->readable) return fail(call, 9, "read");
        if (call.arg(2) > max_io) return "read size exceeds service bound";
        std::vector<std::byte> block(call.arg(2));
        const auto n = ::pread(fd->host, block.data(), block.size(), static_cast<off_t>(fd->offset));
        if (n < 0) return fail(call, 5, "read");
        block.resize(static_cast<std::size_t>(n));
        if (!call.runtime().memory.write(call.arg(1), block)) return "read guest memory fault";
        fd->offset += static_cast<std::uint64_t>(n);
        call.set_result(static_cast<std::uint32_t>(n));
        return std::nullopt;
    });
    // ssize_t write(int fd, const void* buf, size_t count)
    dispatcher.bind("write", [fd_slot, fail](ImportCall& call) -> std::optional<std::string> {
        auto* fd = fd_slot(call.arg(0));
        if (!fd || !fd->writable) return fail(call, 9, "write");
        if (call.arg(2) > max_io) return "write size exceeds service bound";
        std::vector<std::byte> block(call.arg(2));
        if (!call.runtime().memory.read(call.arg(1), block)) return "write guest memory fault";
        if (fd->append) { struct ::stat st{}; if (::fstat(fd->host, &st) == 0) fd->offset = static_cast<std::uint64_t>(st.st_size); }
        const auto n = ::pwrite(fd->host, block.data(), block.size(), static_cast<off_t>(fd->offset));
        if (n < 0) return fail(call, 28, "write");
        fd->offset += static_cast<std::uint64_t>(n);
        call.set_result(static_cast<std::uint32_t>(n));
        return std::nullopt;
    });
    // off_t lseek(int fd, off_t offset, int whence): 32-bit off_t (lseek64 is a separate import)
    dispatcher.bind("lseek", [fd_slot, fail](ImportCall& call) -> std::optional<std::string> {
        auto* fd = fd_slot(call.arg(0));
        if (!fd) return fail(call, 9, "lseek");
        std::int64_t base = 0;
        if (call.arg(2) == 1) base = static_cast<std::int64_t>(fd->offset);
        else if (call.arg(2) == 2) { struct ::stat st{}; if (::fstat(fd->host, &st) != 0) return fail(call, 5, "lseek"); base = st.st_size; }
        else if (call.arg(2) != 0) return fail(call, guest_einval, "lseek");
        const auto target = base + static_cast<std::int32_t>(call.arg(1));
        if (target < 0 || target > 0x7fffffff) return fail(call, guest_einval, "lseek");
        fd->offset = static_cast<std::uint64_t>(target);
        call.set_result(static_cast<std::uint32_t>(target));
        return std::nullopt;
    });
    // int ftruncate(int fd, off_t length)
    dispatcher.bind("ftruncate", [fd_slot, fail](ImportCall& call) -> std::optional<std::string> {
        auto* fd = fd_slot(call.arg(0));
        if (!fd || !fd->writable) return fail(call, 9, "ftruncate");
        if (static_cast<std::int32_t>(call.arg(1)) < 0 || ::ftruncate(fd->host, static_cast<off_t>(call.arg(1))) != 0) return fail(call, guest_einval, "ftruncate");
        call.set_result(0);
        return std::nullopt;
    });
    // int fsync(int fd)
    dispatcher.bind("fsync", [fd_slot, fail](ImportCall& call) -> std::optional<std::string> {
        auto* fd = fd_slot(call.arg(0));
        if (!fd) return fail(call, 9, "fsync");
        call.set_result(::fsync(fd->host) == 0 ? 0 : 0xffffffffU);
        return std::nullopt;
    });
    // mode_t umask(mode_t): returns the previous mask (default 022). Not applied to host mkdir.
    const auto mask = std::make_shared<std::uint32_t>(022);
    dispatcher.bind("umask", [mask](ImportCall& call) -> std::optional<std::string> {
        const auto previous = *mask;
        *mask = call.arg(0) & 0777;  // r0 holds the argument until the result is stored
        call.set_result(previous);
        return std::nullopt;
    });
    // int mkdir(const char* path, mode_t): one level beneath the root; mode is ignored on the host.
    dispatcher.bind("mkdir", [this, set_errno](ImportCall& call) -> std::optional<std::string> {
        std::string path;
        if (!read_guest_string(call.runtime().memory, call.arg(0), 1024, path))
            return "mkdir path is unreadable or too long";
        if (path.empty()) {  // POSIX: ENOENT
            if (trace_) trace_(path, "mkdir", false);
            if (!set_errno(call, guest_enoent)) return "mkdir could not write errno";
            call.set_result(0xffffffffU);
            return std::nullopt;
        }
        std::filesystem::path relative;
        bool escapes = false;
        for (const auto& part : std::filesystem::path(alias(path)).relative_path()) {
            if (part == "..") escapes = true;
            else if (part != ".") relative /= part;
        }
        std::uint32_t code = 0;
        std::error_code ec;
        const auto host = root_ / relative;
        if (escapes) code = guest_eacces;
        else if (relative.empty() || std::filesystem::exists(host, ec)) code = 17;  // EEXIST
        else if (!std::filesystem::is_directory(host.parent_path(), ec)) code = guest_enoent;
        else if (!std::filesystem::create_directory(host, ec)) code = guest_eacces;
        if (trace_) trace_(path, "mkdir", code == 0);
        if (code != 0 && !set_errno(call, code)) return "mkdir could not write errno";
        call.set_result(code == 0 ? 0 : 0xffffffffU);
        return std::nullopt;
    });
    // DIR* opendir(const char* path): listing is read once, sorted, with "." and ".." first.
    // readdir returns a guest `struct dirent` (Bionic ARM32: d_ino u64, d_off i64, d_reclen u16,
    // d_type u8, d_name[256]) held in a host-managed guest region, one slot per open DIR.
    dispatcher.bind("opendir", [this, set_errno](ImportCall& call) -> std::optional<std::string> {
        std::string path;
        if (!read_guest_string(call.runtime().memory, call.arg(0), 1024, path) || path.empty())
            return "opendir path is unreadable, empty or too long";
        std::filesystem::path relative;
        bool escapes = false;
        for (const auto& part : std::filesystem::path(alias(path)).relative_path()) {
            if (part == "..") escapes = true;
            else if (part != ".") relative /= part;
        }
        std::error_code ec;
        const auto host = root_ / relative;
        const bool ok = !escapes && std::filesystem::is_directory(host, ec);
        if (trace_) trace_(path, "opendir", ok);
        if (!ok) {
            if (!set_errno(call, escapes ? guest_eacces : guest_enoent)) return "opendir could not write errno";
            call.set_result(0);
            return std::nullopt;
        }
        std::size_t slot = 0;
        while (slot < dirs_.size() && dirs_[slot].open) ++slot;
        if (slot >= max_dirs) {
            if (!set_errno(call, guest_emfile)) return "opendir could not write errno";
            call.set_result(0);
            return std::nullopt;
        }
        Dir dir;
        dir.open = true;
        dir.entries = {{".", true}, {"..", true}};
        std::vector<std::pair<std::string, bool>> found;
        for (const auto& entry : std::filesystem::directory_iterator(host, ec))
            found.emplace_back(entry.path().filename().string(), entry.is_directory(ec));
        std::sort(found.begin(), found.end());
        dir.entries.insert(dir.entries.end(), found.begin(), found.end());
        if (dirs_.size() <= slot) dirs_.resize(slot + 1);
        dirs_[slot] = std::move(dir);
        auto& memory = call.runtime().memory;
        if (memory.translate(dirent_base, MemoryAccess::write) == nullptr)
            memory.map(dirent_base, std::uint64_t{max_dirs} * dirent_stride, Permissions::read_write);
        call.set_result(dir_base + 4 * static_cast<std::uint32_t>(slot + 1));
        return std::nullopt;
    });
    const auto dir_slot = [this](std::uint32_t handle) -> Dir* {
        if (handle < dir_base + 4 || (handle - dir_base) % 4 != 0 || (handle - dir_base) / 4 > dirs_.size()) return nullptr;
        auto& dir = dirs_[(handle - dir_base) / 4 - 1];
        return dir.open ? &dir : nullptr;
    };
    // struct dirent* readdir(DIR*): NULL at the end (errno untouched).
    dispatcher.bind("readdir", [dir_slot](ImportCall& call) -> std::optional<std::string> {
        auto* dir = dir_slot(call.arg(0));
        if (!dir) return "readdir on an invalid or closed DIR";
        if (dir->next >= dir->entries.size()) { call.set_result(0); return std::nullopt; }
        const auto& [name, is_dir] = dir->entries[dir->next];
        const std::uint32_t at = dirent_base + dirent_stride * static_cast<std::uint32_t>((call.arg(0) - dir_base) / 4 - 1);
        std::array<std::byte, 280> entry{};
        const auto put = [&](unsigned off, std::uint64_t value, unsigned bytes) {
            for (unsigned i = 0; i < bytes; ++i) entry[off + i] = std::byte((value >> (8 * i)) & 255);
        };
        put(0, dir->next + 1, 8);
        put(8, dir->next + 1, 8);
        put(16, 280, 2);
        put(18, is_dir ? 4 : 8, 1);  // DT_DIR / DT_REG
        const auto n = std::min<std::size_t>(name.size(), 255);
        for (std::size_t i = 0; i < n; ++i) entry[19 + i] = std::byte(name[i]);
        if (!call.runtime().memory.write(at, entry)) return "readdir guest memory fault";
        ++dir->next;
        call.set_result(at);
        return std::nullopt;
    });
    // int closedir(DIR*)
    dispatcher.bind("closedir", [dir_slot](ImportCall& call) -> std::optional<std::string> {
        auto* dir = dir_slot(call.arg(0));
        if (!dir) return "closedir on an invalid or closed DIR";
        *dir = Dir{};
        call.set_result(0);
        return std::nullopt;
    });
    // FILE* fopen(const char* path, const char* mode)
    dispatcher.bind("fopen", [this, set_errno](ImportCall& call) -> std::optional<std::string> {
        std::string path, mode;
        if (!read_guest_string(call.runtime().memory, call.arg(0), 1024, path) || path.empty() ||
            !read_guest_string(call.runtime().memory, call.arg(1), 8, mode))
            return "fopen path or mode is unreadable, empty or too long";
        const bool ok_mode = mode == "r" || mode == "rb" || mode == "r+" || mode == "rb+" || mode == "r+b" || mode == "w" ||
                             mode == "wb" || mode == "w+" || mode == "wb+" || mode == "a" || mode == "ab" || mode == "a+";
        if (!ok_mode) return "fopen mode '" + mode + "' is not supported";
        // Resolve beneath the root: lexical normalisation, no parent escapes.
        std::filesystem::path relative;
        for (const auto& part : std::filesystem::path(alias(path)).relative_path()) {
            if (part == "..") { if (trace_) trace_(path, mode, false); call.set_result(0); return set_errno(call, guest_eacces) ? std::nullopt : std::optional<std::string>("fopen could not write errno"); }
            if (part != ".") relative /= part;
        }
        std::FILE* file = nullptr;
        std::uint32_t code = guest_enoent;
        if (files_.size() >= max_open && open_files() >= max_open) code = guest_emfile;
        else if (!relative.empty() && !std::filesystem::is_directory(root_ / relative)) {
            file = std::fopen((root_ / relative).string().c_str(), mode.c_str());
            if (!file) code = errno == ENOENT ? guest_enoent : errno == EACCES ? guest_eacces : guest_einval;
        }
        if (trace_) trace_(path, mode, file != nullptr);
        if (!file) {
            if (!set_errno(call, code)) return "fopen could not write errno";
            call.set_result(0);
            return std::nullopt;
        }
        std::size_t slot = 0;
        while (slot < files_.size() && files_[slot]) ++slot;
        if (slot == files_.size()) files_.push_back(nullptr);
        files_[slot] = file;
        call.set_result(file_base + 4 * static_cast<std::uint32_t>(slot + 1));
        return std::nullopt;
    });
    // int fclose(FILE*)
    dispatcher.bind("fclose", [this, stream](ImportCall& call) -> std::optional<std::string> {
        auto* file = stream(call.arg(0));
        if (!file) return "fclose argument is not an open FILE";
        const int result = std::fclose(file);
        files_[(call.arg(0) - file_base) / 4 - 1] = nullptr;
        call.set_result(result == 0 ? 0 : 0xffffffffU);
        return std::nullopt;
    });
    // size_t fread(void* buf, size_t size, size_t count, FILE*)
    // char* fgets(char* buf, int n, FILE*): NULL at EOF/error when nothing was read
    dispatcher.bind("fgets", [stream](ImportCall& call) -> std::optional<std::string> {
        auto* file = stream(call.arg(2));
        if (!file) return "fgets stream is not an open FILE";
        const auto n = static_cast<std::int32_t>(call.arg(1));
        if (n <= 0 || static_cast<std::uint32_t>(n) > max_io) { call.set_result(0); return std::nullopt; }
        std::vector<std::byte> bytes;
        while (bytes.size() + 1 < static_cast<std::size_t>(n)) {
            const int c = std::fgetc(file);
            if (c == EOF) break;
            bytes.push_back(std::byte(c));
            if (c == '\n') break;
        }
        if (bytes.empty()) { call.set_result(0); return std::nullopt; }
        bytes.push_back(std::byte{0});
        if (!call.runtime().memory.write(call.arg(0), bytes)) return "fgets guest memory fault";
        call.set_result(call.arg(0));
        return std::nullopt;
    });
    dispatcher.bind("fread", [stream](ImportCall& call) -> std::optional<std::string> {
        auto* file = stream(call.arg(3));
        if (!file) return "fread stream is not an open FILE";
        const std::uint64_t total = std::uint64_t{call.arg(1)} * call.arg(2);
        if (total > max_io) return "fread size exceeds service bound";
        std::vector<std::byte> bytes(total);
        const auto got = call.arg(1) == 0 ? 0 : std::fread(bytes.data(), call.arg(1), call.arg(2), file);
        bytes.resize(got * call.arg(1));
        if (!bytes.empty() && !call.runtime().memory.write(call.arg(0), bytes)) return "fread guest memory fault";
        call.set_result(static_cast<std::uint32_t>(got));
        return std::nullopt;
    });
    // size_t fwrite(const void* buf, size_t size, size_t count, FILE*)
    dispatcher.bind("fwrite", [stream](ImportCall& call) -> std::optional<std::string> {
        auto* file = stream(call.arg(3));
        if (!file) return "fwrite stream is not an open FILE";
        const std::uint64_t total = std::uint64_t{call.arg(1)} * call.arg(2);
        if (total > max_io) return "fwrite size exceeds service bound";
        std::vector<std::byte> bytes(total);
        if (total != 0 && !call.runtime().memory.read(call.arg(0), bytes)) return "fwrite guest memory fault";
        call.set_result(call.arg(1) == 0 ? 0 : static_cast<std::uint32_t>(std::fwrite(bytes.data(), call.arg(1), call.arg(2), file)));
        return std::nullopt;
    });
    // int fseek(FILE*, long offset, int whence)
    dispatcher.bind("fseek", [stream](ImportCall& call) -> std::optional<std::string> {
        auto* file = stream(call.arg(0));
        if (!file) return "fseek stream is not an open FILE";
        if (call.arg(2) > 2) return "fseek whence is invalid";
        call.set_result(std::fseek(file, static_cast<std::int32_t>(call.arg(1)), static_cast<int>(call.arg(2))) == 0 ? 0 : 0xffffffffU);
        return std::nullopt;
    });
    // long ftell(FILE*)
    dispatcher.bind("ftell", [stream](ImportCall& call) -> std::optional<std::string> {
        auto* file = stream(call.arg(0));
        if (!file) return "ftell stream is not an open FILE";
        const long at = std::ftell(file);
        call.set_result(at < 0 || at > 0x7fffffffL ? 0xffffffffU : static_cast<std::uint32_t>(at));
        return std::nullopt;
    });
    // void rewind(FILE*), int feof(FILE*), int ferror(FILE*), int fflush(FILE*)
    dispatcher.bind("rewind", [stream](ImportCall& call) -> std::optional<std::string> {
        auto* file = stream(call.arg(0));
        if (!file) return "rewind stream is not an open FILE";
        std::rewind(file);
        return std::nullopt;
    });
    dispatcher.bind("feof", [stream](ImportCall& call) -> std::optional<std::string> {
        auto* file = stream(call.arg(0));
        if (!file) return "feof stream is not an open FILE";
        call.set_result(std::feof(file) ? 1 : 0);
        return std::nullopt;
    });
    dispatcher.bind("ferror", [stream](ImportCall& call) -> std::optional<std::string> {
        auto* file = stream(call.arg(0));
        if (!file) return "ferror stream is not an open FILE";
        call.set_result(std::ferror(file) ? 1 : 0);
        return std::nullopt;
    });
    dispatcher.bind("fflush", [stream](ImportCall& call) -> std::optional<std::string> {
        auto* file = stream(call.arg(0));
        if (!file) return "fflush stream is not an open FILE";
        call.set_result(std::fflush(file) == 0 ? 0 : 0xffffffffU);
        return std::nullopt;
    });
}
}
