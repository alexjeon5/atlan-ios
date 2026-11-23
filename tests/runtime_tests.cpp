// SPDX-License-Identifier: GPL-2.0-or-later
#include "atlan/demo.hpp"
#include <array>
#include <cstring>
#include <iostream>
#include <random>
#include <stdexcept>
#include <string>

using namespace atlan;
namespace {
unsigned checks = 0;
void check(bool condition, const char* description) {
    ++checks;
    if (!condition) throw std::runtime_error(description);
}
template<class Word, std::size_t N>
Runtime program(const std::array<Word, N>& code, bool thumb = false) {
    Runtime runtime;
    runtime.memory.map(0x1000, 4096, Permissions::read_execute,
                       std::as_bytes(std::span{code}));
    runtime.start(thumb ? 0x1001 : 0x1000);
    return runtime;
}
std::uint32_t word(Memory& memory, std::uint32_t address) {
    std::uint32_t value{};
    check(memory.read(address, std::as_writable_bytes(std::span{&value, 1})), "read output");
    return value;
}
void arithmetic() {
    const auto demo = run_demo();
    check(demo.value == 42 && demo.execution.event == Event::supervisor_call &&
          demo.execution.retired == 22, "ARM loop, branch and store");
    auto runtime = program(std::array<std::uint32_t, 1>{0xe0902001}); // adds r2,r0,r1
    std::mt19937 random{0x32a64};
    for (unsigned i = 0; i < 1000; ++i) {
        const std::uint32_t a = i == 0 ? 0x7fffffffU : random();
        const std::uint32_t b = i == 0 ? 1 : random();
        runtime.cpu.registers[0] = a;
        runtime.cpu.registers[1] = b;
        runtime.start(0x1000);
        check(runtime.step().event == Event::retired, "ADDS retires");
        const auto sum = std::uint64_t{a} + b;
        const auto value = static_cast<std::uint32_t>(sum);
        const bool overflow = ((~(a ^ b) & (a ^ value)) >> 31) != 0;
        const std::uint32_t flags = (value & 0x80000000U) | (value == 0 ? 0x40000000U : 0) |
            (sum > UINT32_MAX ? 0x20000000U : 0) | (overflow ? 0x10000000U : 0);
        check(runtime.cpu.registers[2] == value && runtime.cpu.nzcv == flags,
              "32-bit result and NZCV match arithmetic oracle");
    }
    auto reverse = program(std::array<std::uint32_t, 1>{0xe6ff0f31}); // rbit r0,r1
    for (unsigned bit = 0; bit < 32; ++bit) {
        reverse.start(0x1000);
        reverse.cpu.registers[1] = std::uint32_t{1} << bit;
        check(reverse.step().event == Event::retired &&
              reverse.cpu.registers[0] == (std::uint32_t{1} << (31 - bit)),
              "portable RBIT reverses every bit position");
    }
}
bool condition(unsigned cond, unsigned nzcv) {
    const bool n = nzcv & 8, z = nzcv & 4, c = nzcv & 2, v = nzcv & 1;
    const std::array<bool, 15> expected{
        z, !z, c, !c, n, !n, v, !v, c && !z, !c || z,
        n == v, n != v, !z && n == v, z || n != v, true};
    return expected[cond];
}
void predication() {
    for (unsigned cond = 0; cond < 15; ++cond) {
        auto runtime = program(std::array<std::uint32_t, 1>{(cond << 28) | 0x03a00007});
        for (unsigned flags = 0; flags < 16; ++flags) {
            runtime.start(0x1000);
            runtime.cpu.registers[0] = 99;
            runtime.cpu.nzcv = flags << 28;
            check(runtime.step().event == Event::retired, "conditional MOV retires");
            check(runtime.cpu.registers[0] == (condition(cond, flags) ? 7U : 99U),
                  "all condition codes and NZCV combinations");
        }
    }
    // movs r0,#0; cmp r0,#0; ite eq; moveq r1,#7; movne r1,#9; svc #0
    auto runtime = program(std::array<std::uint16_t, 6>{0x2000,0x2800,0xbf0c,0x2107,0x2109,0xdf00}, true);
    check(runtime.run(20).event == Event::supervisor_call && runtime.cpu.registers[1] == 7 &&
          runtime.cpu.a32_itstate == 0, "Thumb IT condition and state advance");
}
void interworking() {
    // Thumb-2 MOVW r0,#0x1234; svc #0
    auto thumb = program(std::array<std::uint16_t, 3>{0xf241,0x2034,0xdf00}, true);
    check(thumb.run(10).event == Event::supervisor_call && thumb.cpu.registers[0] == 0x1234 &&
          thumb.pc() == 0x1006, "Thumb-2 wide instruction and PC width");
    auto runtime = program(std::array<std::uint32_t, 2>{0xe12fff10,0xef000000}); // bx r0; svc
    constexpr std::array<std::uint16_t, 2> target{0x212a,0x4710}; // movs r1,#42; bx r2
    runtime.memory.map(0x2000, 4096, Permissions::read_execute, std::as_bytes(std::span{target}));
    runtime.cpu.registers[0] = 0x2001;
    runtime.cpu.registers[2] = 0x1004;
    check(runtime.run(10).event == Event::supervisor_call && runtime.cpu.registers[1] == 42 &&
          !runtime.thumb(), "ARM to Thumb and back through BX");
}
void memory_faults() {
    auto runtime = program(std::array<std::uint32_t, 1>{0xe5810000}); // str r0,[r1]
    runtime.memory.map(0x2000,4096,Permissions::read_write);
    runtime.cpu.registers[0] = 0x12345678;
    runtime.cpu.registers[1] = 0x2ffe;
    const auto failed_store = runtime.step();
    check(failed_store.event == Event::memory_fault && failed_store.address == 0x3000 &&
          failed_store.access == MemoryAccess::write && runtime.pc() == 0x1000,
          "straddling store reports first inaccessible address");
    check(word(runtime.memory,0x2ffc) == 0, "straddling store has no partial effect");
    runtime.cpu.registers[1] = 0x1000;
    check(runtime.step().event == Event::memory_fault, "code is not writable");
    runtime.start(0x2000);
    check(runtime.step().event == Event::memory_fault, "data is not executable");
    runtime.start(0x1002);
    check(runtime.step().event == Event::pc_alignment, "unaligned ARM PC");

    std::array<std::byte,4096> bytes{};
    bytes[4094] = std::byte{0x40}; bytes[4095] = std::byte{0xf2};
    Runtime wide;
    wide.memory.map(0x1000,4096,Permissions::read_execute,bytes);
    wide.start(0x1fff);
    const auto fetch = wide.step();
    check(fetch.event == Event::memory_fault && fetch.address == 0x2000 &&
          fetch.access == MemoryAccess::execute && wide.pc() == 0x1ffe,
          "Thumb-2 second halfword fetch fault preserves PC");

    Memory memory;
    memory.map(0xfffff000,4096,Permissions::read_write);
    check(memory.translate(0xffffffff,MemoryAccess::write) != nullptr &&
          memory.translate(0x100000000ULL,MemoryAccess::read) == nullptr,
          "ARM32 address space boundary");
    bool rejected = false;
    try { memory.map(0xfffff000,8192,Permissions::read_write); }
    catch (const std::invalid_argument&) { rejected = true; }
    check(rejected, "wrapping mapping rejected");
    rejected = false;
    try { memory.map(0xfffff000,4096,Permissions::read_write); }
    catch (const std::invalid_argument&) { rejected = true; }
    check(rejected, "overlapping mapping rejected");
}
void multiple_transfers_and_exclusives() {
    // ldmia r1!,{r0,r2}: a fault on the second word must preserve all registers.
    auto ldm = program(std::array<std::uint32_t, 1>{0xe8b10005});
    ldm.memory.map(0x2000,4096,Permissions::read_write);
    ldm.cpu.registers[0] = 123; ldm.cpu.registers[1] = 0x2ffc; ldm.cpu.registers[2] = 456;
    check(ldm.step().event == Event::memory_fault && ldm.cpu.registers[0] == 123 &&
          ldm.cpu.registers[2] == 456 && ldm.cpu.registers[1] == 0x2ffc,
          "LDM fault leaves destination and base registers unchanged");
    auto stm = program(std::array<std::uint32_t, 1>{0xe8a10005});
    stm.memory.map(0x2000,4096,Permissions::read_write);
    stm.cpu.registers[0] = 123; stm.cpu.registers[1] = 0x2ffc; stm.cpu.registers[2] = 456;
    check(stm.step().event == Event::memory_fault && word(stm.memory,0x2ffc) == 123 &&
          stm.cpu.registers[1] == 0x2ffc, "STM retains earlier stores on later fault");
    auto exclusive = program(std::array<std::uint32_t, 2>{0xe1910f9f,0xe1812f90});
    exclusive.memory.map(0x2000,4096,Permissions::read_write);
    exclusive.cpu.registers[1] = 0x2000;
    check(exclusive.step().event == Event::retired, "LDREX retires");
    exclusive.cpu.registers[0] = 77;
    check(exclusive.step().event == Event::retired && exclusive.cpu.registers[2] == 0 &&
          word(exclusive.memory,0x2000) == 77, "STREX succeeds with live reservation");
    exclusive.start(0x1004);
    check(exclusive.step().event == Event::retired && exclusive.cpu.registers[2] == 1,
          "STREX fails without reservation");
}
void exits() {
    auto loop = program(std::array<std::uint32_t, 1>{0xeafffffe});
    check(loop.run(0).event == Event::budget_exhausted && loop.pc() == 0x1000,
          "zero budget executes nothing");
    const auto bounded = loop.run(100);
    check(bounded.event == Event::budget_exhausted && bounded.retired == 100,
          "infinite loop bounded by instruction budget");
    auto svc = program(std::array<std::uint32_t, 1>{0xef001234});
    const auto trapped = svc.step();
    check(trapped.event == Event::supervisor_call && trapped.syndrome == 0x1234 &&
          svc.pc() == 0x1004, "SVC returns to host without invoking host syscall");
    auto udf = program(std::array<std::uint32_t, 1>{0xe7f000f0});
    check(udf.step().event == Event::undefined_instruction && udf.pc() == 0x1000,
          "undefined instruction preserves PC");
    auto vector = program(std::array<std::uint32_t, 1>{0xf2200840}); // vadd.i32 q0,q0,q0 (Advanced SIMD)
    check(vector.step().event == Event::unsupported_instruction && vector.pc() == 0x1000,
          "unimported VFP instruction fails explicitly");
}
}
int main() {
    try {
        arithmetic(); predication(); interworking(); memory_faults();
        multiple_transfers_and_exclusives(); exits();
        std::cout << checks << " correctness checks passed\n";
    } catch (const std::exception& error) {
        std::cerr << "check " << checks << ": " << error.what() << '\n';
        return 1;
    }
}
