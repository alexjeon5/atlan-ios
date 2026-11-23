// SPDX-License-Identifier: GPL-2.0-or-later
#include "atlan/demo.hpp"
#include <array>
#include <span>
#include <stdexcept>
namespace atlan {
DemoResult run_demo() {
    // mov r0,#0; mov r1,#6; add r0,r0,#7; subs r1,r1,#1;
    // bne -16; str r0,[r2]; svc #0
    constexpr std::array<std::uint32_t, 7> code{
        0xe3a00000, 0xe3a01006, 0xe2800007, 0xe2511001,
        0x1afffffc, 0xe5820000, 0xef000000};
    Runtime runtime;
    runtime.memory.map(0x1000, Memory::page_size, Permissions::read_execute,
                       std::as_bytes(std::span{code}));
    runtime.memory.map(0x2000, Memory::page_size, Permissions::read_write);
    runtime.cpu.registers[2] = 0x2000;
    runtime.start(0x1000);
    const auto result = runtime.run(100);
    std::uint32_t value = 0;
    if (!runtime.memory.read(0x2000, std::as_writable_bytes(std::span{&value, 1})))
        throw std::runtime_error("demo output is unreadable");
    return {value, result};
}
}
