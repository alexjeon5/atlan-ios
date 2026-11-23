// SPDX-License-Identifier: GPL-2.0-or-later
// Instruction words were assembled independently with an ARM assembler; the
// expected values are computed by hand from IEEE-754 arithmetic.
#include "atlan/runtime.hpp"
#include <array>
#include <cstring>
#include <iostream>
#include <span>
#include <stdexcept>

namespace {
unsigned checks{};
void check(bool result, const char* message) {
    ++checks;
    if (!result) throw std::runtime_error(message);
}
constexpr std::array<std::uint32_t,22> float_ops{0xee000a10, 0xee001a90, 0xee301a20, 0xee701a60, 0xee202a20, 0xeec02a20, 0xee112a10, 0xee123a90, 0xeebd3ac2, 0xee134a10, 0xeeb87ac3, 0xeeb15a40, 0xeef05ac5, 0xeeb16ac2, 0xeefc7ac5, 0xee001a20, 0xee508a20, 0xee175a90, 0xee156a90, 0xee167a10, 0xee118a10, 0xee189a90};
constexpr std::array<std::uint32_t,13> double_ops{0xec410b14, 0xec432b15, 0xee346b05, 0xee247b05, 0xec554b16, 0xec576b17, 0xeef76bc6, 0xee168a90, 0xeeb78ae6, 0xeebdabc7, 0xee1a9a10, 0xee849b05, 0xec5bab19};
constexpr std::array<std::uint32_t,10> compare{0xee000a10, 0xee001a90, 0xeeb40ae0, 0xeef1fa10, 0xb3a08001, 0xa3a08002, 0xeeb74a00, 0xeef84a04, 0xee146a10, 0xee147a90};
constexpr std::array<std::uint32_t,5> memory{0xed2d8b04, 0xecbdab04, 0xed8d0a00, 0xed9d8a00, 0xee182a10};
constexpr std::array<std::uint32_t,4> unsupported{0xf2200840, 0xeee10a10, 0xee300a81, 0x0eb00a60};

constexpr std::array<std::uint32_t,2> faults{0xed900a00, 0xed800a00};

atlan::Runtime program(std::span<const std::uint32_t> words) {
    atlan::Runtime runtime;
    runtime.memory.map(0x1000, 0x1000, atlan::Permissions::read_write);
    runtime.memory.map(0x70000000, 0x2000, atlan::Permissions::read_write);
    for (std::size_t i = 0; i < words.size(); ++i) {
        std::array<std::byte, 4> bytes{};
        for (unsigned b = 0; b < 4; ++b) bytes[b] = std::byte((words[i] >> (8 * b)) & 255);
        check(runtime.memory.write(0x1000 + 4 * static_cast<std::uint32_t>(i), bytes), "load program");
    }
    runtime.memory.protect(0x1000, 0x1000, atlan::Permissions::read_execute);
    runtime.cpu.registers[13] = 0x70001000;
    runtime.start(0x1000);
    return runtime;
}
void run(atlan::Runtime& runtime, std::size_t count) {
    for (std::size_t i = 0; i < count; ++i)
        check(runtime.step().event == atlan::Event::retired, "VFP instruction retires");
}
std::uint64_t d_bits(atlan::Runtime& runtime, unsigned n) {
    std::uint64_t v; std::memcpy(&v, reinterpret_cast<std::byte*>(runtime.cpu.vectors.data()) + 8 * n, 8); return v;
}
void set_d_bits(atlan::Runtime& runtime, unsigned n, std::uint64_t v) {
    std::memcpy(reinterpret_cast<std::byte*>(runtime.cpu.vectors.data()) + 8 * n, &v, 8);
}
void single_precision() {
    auto r = program(float_ops);
    r.cpu.registers[0] = 0x40c00000; r.cpu.registers[1] = 0x3fc00000;  // 6.0f, 1.5f
    run(r, float_ops.size());
    check(r.cpu.registers[2] == 0x40f00000, "vadd.f32 6+1.5");
    check(r.cpu.registers[3] == 0x40800000, "vdiv.f32 6/1.5");
    check(r.cpu.registers[4] == 9, "vcvt.s32.f32 of a product");
    check(r.cpu.registers[5] == 0, "vcvt.u32.f32 saturates a negative to zero");
    check(r.cpu.registers[6] == 0x40c00000, "vneg then vabs");
    check(r.cpu.registers[7] == 0x40400000, "vsqrt.f32 of 9");
    check(r.cpu.registers[8] == 0x41840000, "vmla.f32 accumulates");
    check(r.cpu.registers[9] == 0x41100000, "vnmls.f32 negates the accumulator");
    check(r.pc() == 0x1000 + 4 * float_ops.size(), "PC advances by four");
}
void double_precision() {
    auto r = program(double_ops);
    r.cpu.registers[0] = 0; r.cpu.registers[1] = 0x40180000;  // 6.0
    r.cpu.registers[2] = 0; r.cpu.registers[3] = 0x40000000;  // 2.0
    run(r, double_ops.size());
    check(r.cpu.registers[4] == 0 && r.cpu.registers[5] == 0x40200000, "vadd.f64 and 64-bit vmov");
    check(r.cpu.registers[6] == 0 && r.cpu.registers[7] == 0x40280000, "vmul.f64");
    check(r.cpu.registers[8] == 0x41000000, "vcvt.f32.f64");
    check(r.cpu.registers[9] == 12, "vcvt.s32.f64");
    check(r.cpu.registers[10] == 0 && r.cpu.registers[11] == 0x40080000, "vdiv.f64");
    check(d_bits(r, 8) == 0x4020000000000000ULL, "vcvt.f64.f32 widens exactly");
}
void compare_and_status() {
    auto r = program(compare);
    r.cpu.registers[0] = 0x3f800000; r.cpu.registers[1] = 0x40000000;  // 1.0f < 2.0f
    run(r, compare.size());
    check(r.cpu.registers[8] == 1, "vcmpe less sets N, copied by vmrs, taken by LT");
    check(r.cpu.registers[6] == 0x3f800000 && r.cpu.registers[7] == 0xc0200000, "vmov immediate 1.0 and -2.5");
    auto s = program(compare);
    s.cpu.registers[0] = 0x40000000; s.cpu.registers[1] = 0x3f800000;
    run(s, compare.size());
    check(s.cpu.registers[8] == 2, "vcmpe greater takes GE");
    auto n = program(compare);
    n.cpu.registers[0] = 0x7fc00000; n.cpu.registers[1] = 0x3f800000;
    run(n, 3);
    check((n.cpu.fpscr >> 28) == 3 && (n.cpu.fpscr & 1), "unordered compare sets C and V and raises IOC for vcmpe");
}
void memory_forms() {
    auto r = program(memory);
    set_d_bits(r, 8, 0x1111222233334444ULL); set_d_bits(r, 9, 0x5555666677778888ULL);
    const std::uint32_t pattern = 0xdeadbeef;
    std::memcpy(static_cast<void*>(r.cpu.vectors.data()), &pattern, 4);
    run(r, memory.size());
    check(d_bits(r, 10) == 0x1111222233334444ULL && d_bits(r, 11) == 0x5555666677778888ULL, "vpush then vpop restores registers");
    check(r.cpu.registers[13] == 0x70001000, "push and pop writeback leaves SP unchanged");
    check(r.cpu.registers[2] == 0xdeadbeef, "vstr then vldr round trip");
    auto f = program(faults);
    f.cpu.registers[0] = 0x90000000;
    auto result = f.step();
    check(result.event == atlan::Event::memory_fault && result.address == 0x90000000 &&
          result.access == atlan::MemoryAccess::read && f.pc() == 0x1000, "vldr to unmapped memory faults without advancing");
    f.cpu.registers[0] = 0x70000002;
    result = f.step();
    check(result.event == atlan::Event::memory_fault && result.alignment, "misaligned vldr reports alignment");
    f.cpu.registers[0] = 0x90000000;
    f.start(0x1004);
    result = f.step();
    check(result.event == atlan::Event::memory_fault && result.access == atlan::MemoryAccess::write, "vstr to unmapped memory faults");
}
void explicit_limits() {
    auto r = program(unsupported);
    auto result = r.step();
    check(result.event == atlan::Event::unsupported_instruction && r.pc() == 0x1000, "Advanced SIMD still fails explicitly");
    r.start(0x1004);
    r.cpu.registers[0] = 0x03000000;  // flush-to-zero and default-NaN
    check(r.step().event == atlan::Event::retired, "vmsr accepts FPSCR writes");
    result = r.step();
    check(result.event == atlan::Event::unsupported_instruction && result.detail != nullptr,
          "arithmetic refuses unimplemented FPSCR modes");
    r.cpu.nzcv = 0;
    r.start(0x100c);
    check(r.step().event == atlan::Event::retired && r.pc() == 0x1010, "failed condition retires without executing");
}
}
int main() {
    try {
        single_precision();
        double_precision();
        compare_and_status();
        memory_forms();
        explicit_limits();
    } catch (const std::exception& error) {
        std::cerr << "vfp test failed: " << error.what() << '\n';
        return 1;
    }
    std::cout << "vfp checks passed: " << checks << '\n';
}
