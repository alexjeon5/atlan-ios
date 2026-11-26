// SPDX-License-Identifier: GPL-2.0-or-later
#include "atlan/system_services.hpp"
#include <array>
#include <chrono>
#include <cstring>

namespace atlan {
namespace {
bool put32(Memory& memory, std::uint32_t at, std::uint32_t value) {
    std::array<std::byte, 4> bytes{};
    for (unsigned i = 0; i < 4; ++i) bytes[i] = std::byte((value >> (8 * i)) & 255U);
    return memory.write(at, bytes);
}
bool put64(Memory& memory, std::uint32_t at, std::uint64_t value) {
    return put32(memory, at, static_cast<std::uint32_t>(value)) && put32(memory, at + 4, static_cast<std::uint32_t>(value >> 32));
}
}

void install_system_services(ImportDispatcher& dispatcher, std::uint32_t errno_address) {
    const auto fail = [errno_address](ImportCall& call, std::uint32_t code, const char* what) -> std::optional<std::string> {
        if (errno_address != 0 && !put32(call.runtime().memory, errno_address, code)) return std::string(what) + " could not write errno";
        call.set_result(0xffffffffU);
        return std::nullopt;
    };
    // int sysinfo(struct sysinfo*): 64 bytes on 32-bit Linux; memory figures of a 2 GB device with 1 GB free.
    dispatcher.bind("sysinfo", [](ImportCall& call) -> std::optional<std::string> {
        auto& memory = call.runtime().memory;
        const std::uint32_t at = call.arg(0);
        std::array<std::byte, 64> zero{};
        if (!memory.write(at, zero)) return "sysinfo buffer is not writable";
        static const auto started = std::chrono::steady_clock::now();
        const auto uptime = std::chrono::duration_cast<std::chrono::seconds>(std::chrono::steady_clock::now() - started).count();
        (void)put32(memory, at, static_cast<std::uint32_t>(uptime));
        (void)put32(memory, at + 16, 0x80000000U);  // totalram
        (void)put32(memory, at + 20, 0x40000000U);  // freeram
        (void)put32(memory, at + 52, 1);            // mem_unit (after procs, pad, totalhigh, freehigh)
        call.set_result(0);
        return std::nullopt;
    });
    // int statfs(const char*, struct statfs*): bionic's 32-bit ARM layout (84 bytes); 64 GB with 32 GB free.
    dispatcher.bind("statfs", [](ImportCall& call) -> std::optional<std::string> {
        auto& memory = call.runtime().memory;
        const std::uint32_t at = call.arg(1);
        std::array<std::byte, 84> zero{};
        if (!memory.write(at, zero)) return "statfs buffer is not writable";
        constexpr std::uint32_t block = 4096;
        (void)put32(memory, at, 0xef53);  // f_type: ext4
        (void)put32(memory, at + 4, block);
        (void)put64(memory, at + 8, (64ULL << 30) / block);   // f_blocks
        (void)put64(memory, at + 16, (32ULL << 30) / block);  // f_bfree
        (void)put64(memory, at + 24, (32ULL << 30) / block);  // f_bavail
        (void)put64(memory, at + 32, 1U << 20);               // f_files
        (void)put64(memory, at + 40, 1U << 19);               // f_ffree
        (void)put32(memory, at + 56, 255);                    // f_namelen
        (void)put32(memory, at + 60, block);                  // f_frsize
        call.set_result(0);
        return std::nullopt;
    });
    dispatcher.bind("settimeofday", [fail](ImportCall& call) { return fail(call, 1, "settimeofday"); });
    dispatcher.bind("readlink", [fail](ImportCall& call) { return fail(call, 22, "readlink"); });
    // int pthread_getschedparam(pthread_t, int* policy, struct sched_param*): SCHED_OTHER, priority 0.
    dispatcher.bind("pthread_getschedparam", [](ImportCall& call) -> std::optional<std::string> {
        if ((call.arg(1) != 0 && !put32(call.runtime().memory, call.arg(1), 0)) ||
            (call.arg(2) != 0 && !put32(call.runtime().memory, call.arg(2), 0)))
            return "pthread_getschedparam output is not writable";
        call.set_result(0);
        return std::nullopt;
    });
    // int fgetc(FILE*): fread(&byte, 1, 1, stream) into the caller's stack red zone, then the byte or EOF.
    for (const char* name : {"fgetc", "getc"}) {
        dispatcher.bind(name, [&dispatcher, name](ImportCall& call) -> std::optional<std::string> {
            const auto* fread = dispatcher.service("fread");
            if (fread == nullptr) return std::string(name) + " needs the fread service";
            auto& cpu = call.runtime().cpu;
            const std::uint32_t stream = call.arg(0);
            const std::uint32_t byte_at = static_cast<std::uint32_t>(cpu.registers[13]) - 64;  // below sp, unused by the caller
            cpu.registers[0] = byte_at;
            cpu.registers[1] = 1;
            cpu.registers[2] = 1;
            cpu.registers[3] = stream;
            if (auto error = (*fread)(call)) return error;
            std::array<std::byte, 1> byte{};
            if (cpu.registers[0] != 1 || !call.runtime().memory.read(byte_at, byte)) { call.set_result(0xffffffffU); return std::nullopt; }
            call.set_result(std::to_integer<std::uint32_t>(byte[0]));
            return std::nullopt;
        });
    }
}
}
