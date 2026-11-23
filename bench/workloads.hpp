// SPDX-License-Identifier: GPL-2.0-or-later
// Synthetic guest workloads shared by the benchmark and the differential tests.
// Every program is hand-encoded, public, and ends with SVC #0.
#pragma once
#include "atlan/runtime.hpp"
#include <cstdint>
#include <span>
#include <string_view>
#include <vector>

namespace atlan::bench {
inline constexpr std::uint32_t code_base = 0x10000;
inline constexpr std::uint32_t data_base = 0x20000;
inline constexpr std::uint32_t data_size = 0x4000;
inline constexpr std::uint32_t stack_top = 0x30000;

struct Workload {
    std::string_view name;
    bool thumb;
    std::uint32_t instructions_per_iteration;  // guest instructions retired per loop pass
    std::vector<std::uint32_t> code;           // A32 words, or Thumb halfwords packed little-endian
};

// r0 = iteration count on entry. r5 = data_base. Results end up in r1-r4, r8, memory.
inline std::vector<Workload> workloads() {
    std::vector<Workload> list;
    list.push_back({"alu", false, 6, {
        0xe0811000,  // add r1,r1,r0
        0xe0222001,  // eor r2,r2,r1
        0xe1a03182,  // mov r3,r2,lsl #3
        0xe1844003,  // orr r4,r4,r3
        0xe2500001,  // subs r0,r0,#1
        0x1afffff9,  // bne loop
        0xef000000}});
    list.push_back({"memory", false, 6, {
        0xe20060ff,  // and r6,r0,#255
        0xe7850106,  // str r0,[r5,r6,lsl #2]
        0xe7957106,  // ldr r7,[r5,r6,lsl #2]
        0xe0888007,  // add r8,r8,r7
        0xe2500001,  // subs r0,r0,#1
        0x1afffff9,  // bne loop
        0xef000000}});
    list.push_back({"callret", false, 7, {
        0xeb000002,  // bl f
        0xe2500001,  // subs r0,r0,#1
        0x1afffffc,  // bne loop
        0xef000000,  // svc #0
        0xe2811003,  // f: add r1,r1,#3
        0xe0211000,  // eor r1,r1,r0
        0xe12fff1e}});  // bx lr
    list.push_back({"vfp", false, 5, {
        0xee300a20,  // vadd.f32 s0,s0,s1
        0xee211a20,  // vmul.f32 s2,s2,s1
        0xe2500001,  // subs r0,r0,#1
        0xe1a00000,  // nop (mov r0,r0)
        0x1afffffa,  // bne loop
        0xef000000}});
    // Thumb: halfwords packed two per word, low halfword first.
    const std::uint16_t t[] = {
        0x1809,  // adds r1,r1,r0
        0x404a,  // eors r2,r1
        0x00d3,  // lsls r3,r2,#3
        0x431c,  // orrs r4,r3
        0x3801,  // subs r0,#1
        0xd1f9,  // bne loop
        0xdf00, 0xbf00};  // svc #0, pad
    std::vector<std::uint32_t> packed;
    for (std::size_t i = 0; i < std::size(t); i += 2) packed.push_back(t[i] | std::uint32_t{t[i + 1]} << 16);
    list.push_back({"thumb", true, 6, std::move(packed)});
    return list;
}

inline Runtime make(const Workload& w, std::uint32_t iterations) {
    Runtime runtime;
    std::vector<std::byte> bytes(w.code.size() * 4);
    for (std::size_t i = 0; i < w.code.size(); ++i)
        for (unsigned b = 0; b < 4; ++b) bytes[i * 4 + b] = std::byte(w.code[i] >> (8 * b));
    runtime.memory.map(code_base, 0x1000, Permissions::read_execute, bytes);
    runtime.memory.map(data_base, data_size, Permissions::read_write);
    runtime.memory.map(stack_top - 0x1000, 0x1000, Permissions::read_write);
    runtime.cpu.registers[0] = iterations;
    runtime.cpu.registers[5] = data_base;
    runtime.cpu.registers[13] = stack_top;
    runtime.start(w.thumb ? code_base | 1U : code_base);
    return runtime;
}
}
