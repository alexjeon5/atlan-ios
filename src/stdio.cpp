// SPDX-License-Identifier: GPL-2.0-or-later
#include "atlan/stdio.hpp"
#include "atlan/abi.hpp"
#include <array>
#include <algorithm>
#include <cerrno>
#include <memory>

namespace atlan {
namespace {
constexpr std::uint32_t file_base = 0xe4000000;
constexpr std::uint32_t guest_enoent = 2, guest_eacces = 13, guest_einval = 22, guest_emfile = 24;
constexpr std::uint32_t dir_base = 0xe5000000, dirent_base = 0xe5100000, dirent_stride = 512;
constexpr std::uint32_t max_io = 16U * 1024 * 1024;
}
GuestStdio::~GuestStdio() {
    for (auto* file : files_) if (file) std::fclose(file);
}
std::size_t GuestStdio::open_files() const noexcept {
    std::size_t count = 0;
    for (auto* file : files_) count += file != nullptr;
    return count;
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
            for (const auto& part : std::filesystem::path(path).relative_path()) {
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
    // mode_t umask(mode_t): returns the previous mask (default 022). Not applied to host mkdir.
    const auto mask = std::make_shared<std::uint32_t>(022);
    dispatcher.bind("umask", [mask](ImportCall& call) -> std::optional<std::string> {
        call.set_result(*mask);
        *mask = call.arg(0) & 0777;
        return std::nullopt;
    });
    // int mkdir(const char* path, mode_t): one level beneath the root; mode is ignored on the host.
    dispatcher.bind("mkdir", [this, set_errno](ImportCall& call) -> std::optional<std::string> {
        std::string path;
        if (!read_guest_string(call.runtime().memory, call.arg(0), 1024, path) || path.empty())
            return "mkdir path is unreadable, empty or too long";
        std::filesystem::path relative;
        bool escapes = false;
        for (const auto& part : std::filesystem::path(path).relative_path()) {
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
        for (const auto& part : std::filesystem::path(path).relative_path()) {
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
    dispatcher.bind("readdir", [this, dir_slot](ImportCall& call) -> std::optional<std::string> {
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
        for (const auto& part : std::filesystem::path(path).relative_path()) {
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
