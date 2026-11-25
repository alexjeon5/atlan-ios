// SPDX-License-Identifier: GPL-2.0-or-later
// GuestProfiler on a synthetic guest: direct calls in a loop, an import call through a register, a tail call.
#include "atlan/imports.hpp"
#include "atlan/profiler.hpp"
#include <array>
#include <iostream>
#include <map>
#include <sstream>
#include <stdexcept>
#include <vector>

namespace {
unsigned checks{};
void check(bool result, const char* message) {
    ++checks;
    if (!result) throw std::runtime_error(message);
}
constexpr std::uint32_t code = 0x1000, leaf = 0x1040, tail = 0x1060, stack = 0x80000, sentinel = 0xffff0000, trap = 0xf0000000;
struct Row { std::string name; std::uint64_t calls{}, self{}, inclusive{}; };
}

int main() {
    try {
        atlan::Runtime runtime;
        runtime.memory.map(code, 0x1000, atlan::Permissions::read_write);
        runtime.memory.map(stack - 0x1000, 0x1000, atlan::Permissions::read_write);
        const auto put = [&](std::uint32_t at, std::initializer_list<std::uint32_t> words) {
            for (std::uint32_t word : words) {
                std::array<std::byte, 4> bytes{};
                for (unsigned i = 0; i < 4; ++i) bytes[i] = std::byte((word >> (8 * i)) & 255);
                check(runtime.memory.write(at, bytes), "write guest word");
                at += 4;
            }
        };
        put(code, {0xe92d4010,    // push {r4, lr}
                   0xe3a04003,    // mov r4, #3
                   0xeb00000c,    // 1: bl leaf
                   0xe2544001,    // subs r4, r4, #1
                   0x1afffffc,    // bne 1b
                   0xe59fc00c,    // ldr r12, =trap
                   0xe12fff3c,    // blx r12
                   0xe8bd4010,    // pop {r4, lr}
                   0xea00000e,    // b tail
                   0xe1a00000, trap});
        put(leaf, {0xe2800001, 0xe12fff1e});  // add r0, r0, #1; bx lr
        put(tail, {0xe2800002, 0xe12fff1e});  // add r0, r0, #2; bx lr
        runtime.memory.protect(code, 0x1000, atlan::Permissions::read_execute);

        atlan::ImportDispatcher dispatcher;
        check(dispatcher.bind("service", [](atlan::ImportCall& call) -> std::optional<std::string> {
            call.set_result(call.arg(0) + 10);
            return std::nullopt;
        }), "bind service");
        check(dispatcher.bind_trap(trap, "service"), "bind trap");
        atlan::GuestProfiler profiler;
        profiler.add_function(code, "main");
        profiler.add_function(trap, "import:service");
        dispatcher.set_profiler(&profiler);

        const auto run = [&] {
            const std::array<std::uint32_t, 1> arguments{100};
            const auto outcome = dispatcher.call(runtime, code, arguments, stack, sentinel, 1000);
            check(outcome.stop == atlan::Stop::returned, "profiled call returns");
            check(runtime.cpu.registers[0] == 115, "profiled call computes the same result");
            check(outcome.retired == 23, "profiled call retires the same instructions");
        };
        const auto rows = [&] {
            std::ostringstream out;
            profiler.write(out);
            std::istringstream in(out.str());
            std::map<std::string, Row> functions;
            std::map<std::string, std::uint64_t> edges;
            std::string kind, a, b;
            while (in >> kind >> a >> b) {
                if (kind == "F") {
                    Row row;
                    double self_ms, inclusive_ms;
                    row.name = b;
                    in >> row.calls >> row.self >> self_ms >> row.inclusive >> inclusive_ms;
                    functions[a] = row;
                } else {
                    std::uint64_t calls, inclusive;
                    in >> calls >> inclusive;
                    edges[a + ">" + b] = calls;
                }
            }
            return std::pair{functions, edges};
        };
        const auto expect = [&](unsigned times) {
            const auto [functions, edges] = rows();
            const auto is = [&](const char* address, const char* name, std::uint64_t calls, std::uint64_t self, std::uint64_t inclusive) {
                const auto found = functions.find(address);
                return found != functions.end() && found->second.name == name && found->second.calls == calls * times &&
                       found->second.self == self * times && found->second.inclusive == inclusive * times;
            };
            check(is("00001000", "main", 1, 15, 21), "caller: self and inclusive instructions");
            check(is("00001040", "-", 3, 6, 6), "discovered callee, called in a loop from one site");
            check(is("00001060", "-", 1, 2, 2), "tail-called function");
            check(is("f0000000", "import:service", 1, 0, 0), "import called through a register");
            check(edges.at("00001000>00001040") == 3 * times && edges.at("00001000>f0000000") == times &&
                  edges.at("00000000>00001000") == times && edges.at("00000000>00001060") == times, "call edges");
        };
        run();
        expect(1);
        run();
        expect(2);
        profiler.reset();
        run();
        expect(1);
        check(profiler.summary(1).starts_with("main=15/1"), "summary names the busiest function");

        // Without a profiler the same call gives the same result.
        dispatcher.set_profiler(nullptr);
        const std::array<std::uint32_t, 1> arguments{100};
        const auto plain = dispatcher.call(runtime, code, arguments, stack, sentinel, 1000);
        check(plain.stop == atlan::Stop::returned && runtime.cpu.registers[0] == 115 && plain.retired == 23, "unprofiled call");
        std::cout << "profiler tests passed: " << checks << " checks\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "profiler test failed: " << error.what() << '\n';
        return 1;
    }
}
