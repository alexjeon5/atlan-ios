// SPDX-License-Identifier: GPL-2.0-or-later
#include "atlan/files.hpp"
#include <array>

namespace atlan {
namespace {
constexpr std::uint32_t f_getfd = 1, f_setfd = 2, f_getfl = 3, f_setfl = 4, ebadf = 9;
constexpr std::uint32_t settable_status = 04000;  // O_NONBLOCK on ARM Linux
}
void VirtualFiles::install(ImportDispatcher& dispatcher) {
    // int* __errno(void)
    dispatcher.bind("__errno", [this](ImportCall& call) -> std::optional<std::string> {
        call.set_result(errno_address_);
        return std::nullopt;
    });
    // int fstat(int fd, struct stat*): ARM Bionic stat64 layout (104 bytes). The standard
    // streams are reported as character devices; they are not attached to a host terminal.
    dispatcher.bind("fstat", [this](ImportCall& call) -> std::optional<std::string> {
        auto& memory = call.runtime().memory;
        if (!descriptors_.contains(call.arg(0))) {
            std::array<std::byte, 4> code{std::byte{ebadf}};
            if (!memory.write(errno_address_, code)) return "fstat could not write errno";
            call.set_result(0xffffffffU);
            return std::nullopt;
        }
        std::array<std::byte, 104> stat{};
        const auto put = [&](unsigned at, std::uint32_t value) {
            for (unsigned i = 0; i < 4; ++i) stat[at + i] = std::byte((value >> (8 * i)) & 255);
        };
        put(16, 0020620);  // S_IFCHR | rw--w----
        put(20, 1);        // st_nlink
        put(56, 4096);     // st_blksize
        if (!memory.write(call.arg(1), stat)) return "fstat guest memory fault";
        call.set_result(0);
        return std::nullopt;
    });
    // int fcntl(int fd, int cmd, ...): descriptor and status flags of the virtual table only.
    dispatcher.bind("fcntl", [this](ImportCall& call) -> std::optional<std::string> {
        const auto set_errno = [&](std::uint32_t code) -> std::optional<std::string> {
            std::array<std::byte, 4> bytes{};
            for (unsigned i = 0; i < 4; ++i) bytes[i] = std::byte((code >> (8 * i)) & 255);
            if (!call.runtime().memory.write(errno_address_, bytes)) return "fcntl could not write errno";
            call.set_result(0xffffffffU);
            return std::nullopt;
        };
        const auto found = descriptors_.find(call.arg(0));
        if (found == descriptors_.end()) return set_errno(ebadf);
        auto& descriptor = found->second;
        switch (call.arg(1)) {
        case f_getfd: call.set_result(descriptor.descriptor_flags); return std::nullopt;
        case f_setfd: descriptor.descriptor_flags = call.arg(2) & 1; call.set_result(0); return std::nullopt;
        case f_getfl: call.set_result(descriptor.status_flags); return std::nullopt;
        case f_setfl:
            descriptor.status_flags = (descriptor.status_flags & ~settable_status) | (call.arg(2) & settable_status);
            call.set_result(0);
            return std::nullopt;
        default:
            return "fcntl command " + std::to_string(call.arg(1)) + " is not supported";
        }
    });
}
}
