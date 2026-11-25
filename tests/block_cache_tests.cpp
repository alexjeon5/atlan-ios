// SPDX-License-Identifier: GPL-2.0-or-later
// Differential tests: BlockRunner must be indistinguishable from Runtime::step().
#include "../bench/workloads.hpp"
#include "atlan/block_cache.hpp"
#include "atlan/imports.hpp"
#include "cpu/arm64/aarch32/guest_key.hpp"
#include <cstdio>
#include <cstring>
#include <random>
#include <stdexcept>
#include <string>

using namespace atlan;
namespace {
unsigned checks = 0;
void check(bool condition, const std::string& what) {
    ++checks;
    if (!condition) throw std::runtime_error(what);
}
bool same(const Result& a, const Result& b) {
    return a.event == b.event && a.retired == b.retired && a.address == b.address &&
           a.syndrome == b.syndrome && a.access == b.access && a.alignment == b.alignment &&
           ((a.detail == nullptr) == (b.detail == nullptr)) &&
           (a.detail == nullptr || !std::strcmp(a.detail, b.detail));
}
std::string describe(const Result& r) {
    return std::string(event_name(r.event)) + " retired=" + std::to_string(r.retired) +
           " addr=" + std::to_string(r.address) + " syn=" + std::to_string(r.syndrome);
}
bool same(const CpuState& a, const CpuState& b) {
    return a.registers == b.registers && a.pc == b.pc && a.nzcv == b.nzcv && a.fpscr == b.fpscr &&
           !std::memcmp(a.vectors.data(), b.vectors.data(), sizeof a.vectors) && a.tls == b.tls &&
           a.a32_ge_mask == b.a32_ge_mask && a.a32_tpidruro == b.a32_tpidruro && a.a32_q == b.a32_q &&
           a.a32_itstate == b.a32_itstate && a.a32_cp15ben == b.a32_cp15ben &&
           a.exclusive_address == b.exclusive_address &&  // exclusive_key is a host pointer: differs per Runtime
           (a.exclusive_key != 0) == (b.exclusive_key != 0) &&
           a.exclusive_value == b.exclusive_value && a.exclusive_value2 == b.exclusive_value2 &&
           a.exclusive_version == b.exclusive_version && a.exclusive_size == b.exclusive_size;
}
bool same_memory(Runtime& a, Runtime& b) {
    for (std::uint32_t base : {bench::data_base, bench::stack_top - 0x1000, bench::code_base}) {
        const std::uint32_t size = base == bench::data_base ? bench::data_size : 0x1000;
        std::vector<std::byte> x(size), y(size);
        const bool xa = a.memory.read(base, x), ya = b.memory.read(base, y);
        if (xa != ya || (xa && std::memcmp(x.data(), y.data(), size))) return false;
    }
    return true;
}
void expect_same(Runtime& a, Runtime& b, const Result& ra, const Result& rb, const std::string& what) {
    check(same(ra, rb), what + ": result [" + describe(ra) + "] vs [" + describe(rb) + "]");
    if (!same(a.cpu, b.cpu)) {
        std::string d;
        for (unsigned i = 0; i < 16; ++i) if (a.cpu.registers[i] != b.cpu.registers[i]) d += " r" + std::to_string(i) + "=" + std::to_string(a.cpu.registers[i]) + "/" + std::to_string(b.cpu.registers[i]);
        if (a.cpu.pc != b.cpu.pc) d += " pc=" + std::to_string(a.cpu.pc) + "/" + std::to_string(b.cpu.pc);
        if (a.cpu.nzcv != b.cpu.nzcv) d += " nzcv";
        if (a.cpu.a32_itstate != b.cpu.a32_itstate) d += " it";
        if (a.cpu.exclusive_address != b.cpu.exclusive_address) d += " excl";
        check(false, what + ": cpu state" + d);
    }
    check(same_memory(a, b), what + ": memory");
}

// Slices a run into random budgets so blocks are cut at every offset.
void workload_slices() {
    std::mt19937 random{7};
    for (const auto& w : bench::workloads()) {
        for (std::uint32_t iterations : {1U, 2U, 3U, 50U, 1000U}) {
            auto a = bench::make(w, iterations), b = bench::make(w, iterations);
            BlockRunner runner(b);
            const std::uint64_t total = std::uint64_t{iterations} * w.instructions_per_iteration + 8;
            std::uint64_t spent = 0;
            while (spent < total) {
                const std::uint64_t slice = 1 + random() % 23;
                const auto ra = a.run(slice), rb = runner.run(slice);
                expect_same(a, b, ra, rb, std::string(w.name) + " slice");
                spent += ra.retired;
                if (ra.event != Event::budget_exhausted) break;
            }
            check(a.cpu.registers[0] == 0 || spent >= total, std::string(w.name) + " ran to completion");
        }
        auto a = bench::make(w, 10), b = bench::make(w, 10);
        BlockRunner runner(b);
        const auto ra = a.run(~0ULL >> 1), rb = runner.run(~0ULL >> 1);
        expect_same(a, b, ra, rb, std::string(w.name) + " to svc");
        check(ra.event == Event::supervisor_call, std::string(w.name) + " ends in svc");
        check(runner.stats().blocks_built > 0 && runner.stats().block_entries > 0, "blocks are reused");
    }
}

// With `seams`, pointers cluster around page seams and the ends of the mapping, where the specialised
// handlers must hand over to the library (page-crossing accesses, faults, partial multiple stores).
void randomize(Runtime& r, std::mt19937& random, bool seams = false) {
    for (unsigned i = 0; i < 15; ++i) {
        const auto pick = random() % 4;
        r.cpu.registers[i] = pick == 0 ? bench::data_base + ((random() % 0x3f00) & ~3U)
                           : pick == 1 ? random() % 64 : random();
        if (seams && pick != 1) {
            const std::uint32_t seam = random() % 3 == 0 ? bench::data_base
                                     : random() % 2 ? bench::data_base + bench::data_size : bench::data_base + 0x1000;
            r.cpu.registers[i] = seam - 72 + random() % 144;
        }
    }
    r.cpu.nzcv = random() & 0xf0000000U;
    r.cpu.a32_q = random() & 1;
    r.cpu.a32_ge_mask = random() & 0xf;
    r.cpu.fpscr = random() & 0xf8000000U;
    for (auto& v : r.cpu.vectors) for (auto& b : v.bytes) b = std::byte(random());
}

// One random instruction, executed through both paths from identical state.
// `mask`/`value` force bits of every word (an encoding family the generic random words rarely hit).
void random_single(unsigned count, bool thumb, std::uint32_t seed, bool seams = false, bool vfp = false,
                   std::uint32_t mask = 0, std::uint32_t value = 0) {
    std::mt19937 random{seed};
    Runtime a, b;
    for (auto* r : {&a, &b}) {
        r->memory.map(bench::code_base, 0x2000, Permissions::read_execute);
        r->memory.map(bench::data_base, bench::data_size, Permissions::read_write);
        r->memory.map(bench::stack_top - 0x1000, 0x1000, Permissions::read_write);
    }
    BlockRunner runner(b);
    unsigned events[16] = {};
    for (unsigned n = 0; n < count; ++n) {
        std::uint32_t word = random();
        if (!thumb && random() % 3) word = (word & 0x0fffffffU) | 0xe0000000U;  // mostly AL
        if (thumb && random() % 2) word &= 0xffffU;                            // 16-bit forms
        // Scalar VFP: coprocessor 10 or 11 in the three classes the VFP executor takes.
        if (vfp) word = (word & 0xf0fff0ffU) | (12U + random() % 3) << 24 | (10U + random() % 2) << 8;
        word = (word & ~mask) | value;
        const std::uint32_t at = bench::code_base + (thumb ? 2 * (random() % 8) : 0);
        for (auto* r : {&a, &b}) {
            r->memory.protect(bench::code_base, 0x2000, Permissions::read_write);
            check(r->memory.write(at, std::as_bytes(std::span{&word, 1})), "write code");
            r->memory.protect(bench::code_base, 0x2000, Permissions::read_execute);
        }
        const auto state_seed = random();
        for (auto* r : {&a, &b}) {
            std::mt19937 s{state_seed};
            randomize(*r, s, seams);
            r->cpu.clear_exclusive_monitor();
            if (random() % 8 == 0) r->cpu.exclusive_address = 0;
            r->start(thumb ? at | 1U : at);
            if (thumb) r->cpu.a32_itstate = static_cast<std::uint8_t>(state_seed >> 24) % 3 == 0 ? (state_seed >> 8) & 0xff : 0;
        }
        const auto ra = a.run(1), rb = runner.run(1);
        ++events[static_cast<unsigned>(ra.event)];
        expect_same(a, b, ra, rb, std::string(thumb ? "thumb" : "a32") + " word " + std::to_string(word));
    }
    std::printf("  random %s%s: retired %u, svc %u, undefined %u, fault %u, unsupported %u, budget %u\n",
                thumb ? "thumb" : "a32", vfp ? (seams ? " (vfp, seams)" : " (vfp)") : seams ? " (seams)" : "", events[0], events[1], events[3], events[4], events[6], events[7]);
}

// Random straight-line and branching programs run for a fixed budget.
void random_programs(unsigned count, bool thumb, std::uint32_t seed) {
    std::mt19937 random{seed};
    for (unsigned n = 0; n < count; ++n) {
        Runtime a, b;
        std::vector<std::uint32_t> words(64);
        for (auto& w : words) {
            w = random();
            if (!thumb) w = (w & 0x0fffffffU) | (random() % 4 ? 0xe0000000U : (w & 0xf0000000U));
            // Keep branch forms rare so programs run several instructions.
            if (!thumb && (w & 0x0e000000U) == 0x0a000000U && random() % 4) w = 0xe1a00000;
        }
        std::vector<std::byte> bytes(0x2000);
        std::memcpy(bytes.data(), words.data(), words.size() * 4);
        std::vector<std::byte> data(bench::data_size);
        for (auto& x : data) x = std::byte(random());
        for (auto* r : {&a, &b}) {
            r->memory.map(bench::code_base, 0x2000, Permissions::read_execute, bytes);
            r->memory.map(bench::data_base, bench::data_size, Permissions::read_write, data);
            r->memory.map(bench::stack_top - 0x1000, 0x1000, Permissions::read_write);
        }
        const auto state_seed = random();
        for (auto* r : {&a, &b}) {
            std::mt19937 s{state_seed};
            randomize(*r, s);
            r->start(thumb ? bench::code_base | 1U : bench::code_base);
        }
        BlockRunner runner(b);
        std::uint64_t spent = 0;
        while (spent < 200) {  // sliced, so resumption after any event-free prefix is covered
            const std::uint64_t slice = 1 + random() % 17;
            const auto ra = a.run(slice), rb = runner.run(slice);
            expect_same(a, b, ra, rb, std::string(thumb ? "thumb" : "a32") + " program " + std::to_string(n));
            spent += slice;
            if (ra.event != Event::budget_exhausted) break;
        }
    }
}

// The import dispatcher must stop, count and return the same with and without the block cache, for every budget.
void dispatcher_paths() {
    const bench::Workload program{"imports", false, 0, {
        0xe92d4010,  // push {r4, lr}
        0xe1a04000,  // mov r4, r0
        0xe0811004,  // loop: add r1, r1, r4
        0xe1a00004,  // mov r0, r4
        0xeb1ffffa,  // bl trap (unmapped, code_base + 0x800000)
        0xe0811000,  // add r1, r1, r0
        0xe2544001,  // subs r4, r4, #1
        0x1afffff9,  // bne loop
        0xe1a00001,  // mov r0, r1
        0xe8bd8010}};  // pop {r4, pc}
    std::mt19937 random{31};
    for (unsigned n = 0; n < 400; ++n) {
        const std::uint32_t iterations = 1 + random() % 40;
        const std::uint64_t budget = n % 4 == 0 ? 1000000 : random() % (iterations * 6 + 12);
        CallOutcome outcomes[2];
        Runtime runtimes[2] = {bench::make(program, iterations), bench::make(program, iterations)};
        for (unsigned cached = 0; cached < 2; ++cached) {
            ImportDispatcher dispatcher;
            dispatcher.set_block_cache(cached != 0);
            std::uint32_t calls = 0;
            check(dispatcher.bind_trap(bench::code_base + 0x800000, "triple"), "bind trap");
            check(dispatcher.bind("triple", [&](ImportCall& call) -> std::optional<std::string> {
                call.set_result(call.arg(0) * 3 + calls++);
                return std::nullopt;
            }), "bind service");
            const std::uint32_t arguments[] = {iterations};
            outcomes[cached] = dispatcher.call(runtimes[cached], bench::code_base, arguments, bench::stack_top, 0xffff0000, budget);
            check(dispatcher.remaining_budget() == budget - outcomes[cached].retired, "remaining budget");
        }
        check(outcomes[0].stop == outcomes[1].stop && outcomes[0].retired == outcomes[1].retired, "dispatcher stop and count");
        if (n % 4 == 0) check(outcomes[0].stop == Stop::returned, "dispatcher call returns");
        expect_same(runtimes[0], runtimes[1], outcomes[0].cpu, outcomes[1].cpu, "dispatcher paths");
    }
}

void invalidation() {
    // add r0,r0,#1 ; add r0,r0,#1 ; svc, then the middle word is rewritten.
    std::vector<std::uint32_t> code{0xe2800001, 0xe2800001, 0xef000000};
    auto make = [&] {
        Runtime r;
        r.memory.map(bench::code_base, 0x1000, Permissions::read_execute, std::as_bytes(std::span{code}));
        r.start(bench::code_base);
        return r;
    };
    auto a = make(), b = make();
    BlockRunner runner(b);
    expect_same(a, b, a.run(100), runner.run(100), "first run");
    check(a.cpu.registers[0] == 2, "two increments");
    const std::uint32_t patched = 0xe2800005;  // add r0,r0,#5
    for (auto* r : {&a, &b}) {
        r->memory.protect(bench::code_base, 0x1000, Permissions::read_write);
        check(r->memory.write(bench::code_base + 4, std::as_bytes(std::span{&patched, 1})), "patch");
        r->memory.protect(bench::code_base, 0x1000, Permissions::read_execute);
        r->cpu.registers[0] = 0;
        r->start(bench::code_base);
    }
    expect_same(a, b, a.run(100), runner.run(100), "patched run");
    check(a.cpu.registers[0] == 6, "patched code executes, not the stale block");
    // Removing execute permission must fault on the next entry.
    for (auto* r : {&a, &b}) {
        r->memory.protect(bench::code_base, 0x1000, Permissions::read);
        r->start(bench::code_base);
    }
    const auto ra = a.run(10), rb = runner.run(10);
    expect_same(a, b, ra, rb, "execute removed");
    check(ra.event == Event::memory_fault && ra.access == MemoryAccess::execute, "execute fault");
}

void edges() {
    // Code up to the end of a page; the next page is unmapped.
    std::vector<std::byte> page(0x1000);
    const std::uint32_t nop = 0xe1a00000;
    for (std::size_t i = 0; i < page.size(); i += 4) std::memcpy(&page[i], &nop, 4);
    auto make = [&](bool thumb) {
        Runtime r;
        r.memory.map(bench::code_base, 0x1000, Permissions::read_execute, page);
        r.start(bench::code_base + 0x1000 - 8 + (thumb ? 1 : 0));
        return r;
    };
    for (bool thumb : {false, true}) {
        auto a = make(thumb), b = make(thumb);
        BlockRunner runner(b);
        expect_same(a, b, a.run(50), runner.run(50), thumb ? "page end thumb" : "page end a32");
    }
    // 32-bit Thumb instruction whose second halfword is unmapped.
    {
        std::vector<std::byte> p2(0x1000);
        const std::uint16_t first = 0xf04f;  // mov.w prefix
        std::memcpy(&p2[0xffe], &first, 2);
        auto make2 = [&] {
            Runtime r;
            r.memory.map(bench::code_base, 0x1000, Permissions::read_execute, p2);
            r.start((bench::code_base + 0xffe) | 1U);
            return r;
        };
        auto a = make2(), b = make2();
        BlockRunner runner(b);
        const auto ra = a.run(5), rb = runner.run(5);
        expect_same(a, b, ra, rb, "wide thumb at seam");
        check(ra.event == Event::memory_fault && ra.address == bench::code_base + 0x1000, "seam fault address");
    }
    // Unmapped PC and misaligned A32 PC.
    {
        Runtime a, b;
        BlockRunner runner(b);
        for (auto* r : {&a, &b}) r->start(0x5000);
        expect_same(a, b, a.run(5), runner.run(5), "unmapped pc");
        for (auto* r : {&a, &b}) r->cpu.pc = uml::cpu::arm64::aarch32::a32_key(0x5002);
        expect_same(a, b, a.run(5), runner.run(5), "misaligned pc");
    }
    // Budget edge cases.
    {
        auto w = bench::workloads()[0];
        auto a = bench::make(w, 5), b = bench::make(w, 5);
        BlockRunner runner(b);
        expect_same(a, b, a.run(0), runner.run(0), "zero budget");
        expect_same(a, b, a.run(1), runner.run(1), "one instruction");
        const auto r = runner.step_block(1000);
        check(r.event == Event::retired && r.retired >= 1 && r.retired <= 6, "step_block stays within a block");
        (void)a.run(r.retired);
    }
}
}

// Sanitizer builds run the same checks about 100x slower; shrink the random sets there.
#if defined(__SANITIZE_ADDRESS__)
constexpr unsigned scale = 30;
#elif defined(__has_feature)
#if __has_feature(address_sanitizer)
constexpr unsigned scale = 30;
#else
constexpr unsigned scale = 1;
#endif
#else
constexpr unsigned scale = 1;
#endif

int main() {
    try {
        workload_slices();
        random_single(30000 / scale, false, 11);
        random_single(30000 / scale, true, 12);
        random_single(30000 / scale, false, 13, true);
        random_single(30000 / scale, true, 14, true);
        random_single(60000 / scale, false, 15, false, true);
        random_single(60000 / scale, false, 16, true, true);
        // Long and halfword multiplies, LDRD/STRD (immediate and register), A32 and T32.
        random_single(8000 / scale, false, 31, false, false, 0x0f8000f0U, 0x00800090U);  // UMULL/UMLAL/SMULL/SMLAL
        random_single(8000 / scale, false, 32, false, false, 0x0f900090U, 0x01000080U);  // SMLA<x><y>, SMUL<x><y> and neighbours
        random_single(8000 / scale, false, 33, false, false, 0x0e0000d0U, 0x000000d0U);  // LDRD/STRD and neighbours
        random_single(8000 / scale, false, 34, true, false, 0x0e0000d0U, 0x000000d0U);
        random_single(8000 / scale, true, 35, false, false, 0xfe400000U, 0xe8400000U);   // T32 LDRD/STRD (immediate)
        random_single(8000 / scale, true, 36, true, false, 0xfe400000U, 0xe8400000U);
        random_single(8000 / scale, true, 37, false, false, 0xff800000U, 0xfb800000U);   // T32 long multiplies
        random_single(8000 / scale, true, 38, false, false, 0xfff00000U, 0xfb100000U);   // T32 SMLA<x><y>/SMUL<x><y>
        random_programs(1500 / scale, false, 21);
        random_programs(1500 / scale, true, 22);
        dispatcher_paths();
        invalidation();
        edges();
    } catch (const std::exception& error) {
        std::fprintf(stderr, "FAILED: %s\n", error.what());
        return 1;
    }
    std::printf("block cache differential checks passed: %u\n", checks);
}
