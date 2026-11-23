// SPDX-License-Identifier: GPL-2.0-or-later
// Interpreter throughput on synthetic public workloads. Private measurements
// against the original library belong under private/ and use atlan-elf.
#include "workloads.hpp"
#include "atlan/block_cache.hpp"
#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>

using namespace atlan;
using Clock = std::chrono::steady_clock;

namespace {
struct Sample { double seconds; std::uint64_t retired; Event event; std::uint64_t digest; };

std::uint64_t digest(Runtime& r) {
    std::uint64_t h = 1469598103934665603ULL;
    auto mix = [&](std::uint64_t v) { h = (h ^ v) * 1099511628211ULL; };
    for (auto reg : r.cpu.registers) mix(reg);
    mix(r.cpu.pc); mix(r.cpu.nzcv); mix(r.cpu.fpscr);
    for (auto& v : r.cpu.vectors) for (auto b : v.bytes) mix(std::to_integer<unsigned>(b));
    for (std::uint32_t a = bench::data_base; a < bench::data_base + bench::data_size; a += 4) {
        std::uint32_t w{};
        if (r.memory.read(a, std::as_writable_bytes(std::span{&w, 1}))) mix(w);
    }
    return h;
}

Sample run_once(const bench::Workload& w, std::uint32_t iterations, bool cached) {
    auto runtime = bench::make(w, iterations);
    BlockRunner runner(runtime);
    const auto budget = std::uint64_t{iterations} * w.instructions_per_iteration + 16;
    const auto start = Clock::now();
    const auto result = cached ? runner.run(budget) : runtime.run(budget);
    const auto stop = Clock::now();
    return {std::chrono::duration<double>(stop - start).count(), result.retired, result.event, digest(runtime)};
}
}

int main(int argc, char** argv) {
    std::uint32_t iterations = 2'000'000;
    unsigned repeats = 5;
    bool csv = false;
    std::string only, mode = "both";
    for (int i = 1; i < argc; ++i) {
        if (!std::strcmp(argv[i], "--iterations") && i + 1 < argc) iterations = std::strtoul(argv[++i], nullptr, 10);
        else if (!std::strcmp(argv[i], "--repeats") && i + 1 < argc) repeats = std::strtoul(argv[++i], nullptr, 10);
        else if (!std::strcmp(argv[i], "--workload") && i + 1 < argc) only = argv[++i];
        else if (!std::strcmp(argv[i], "--mode") && i + 1 < argc) mode = argv[++i];
        else if (!std::strcmp(argv[i], "--csv")) csv = true;
        else { std::fprintf(stderr, "usage: atlan-bench [--iterations N] [--repeats N] [--workload NAME] [--mode interp|cached|both] [--csv]\n"); return 2; }
    }
    if (csv) std::puts("workload,mode,retired,best_mips,median_mips,digest");
    else std::printf("%-8s %-8s %12s %10s %10s  %s\n", "workload", "mode", "retired", "best MIPS", "median", "digest");
    int status = 0;
    for (const auto& w : bench::workloads()) {
        if (!only.empty() && only != w.name) continue;
        std::uint64_t reference = 0;
        for (const bool cached : {false, true}) {
            if (mode != "both" && mode != (cached ? "cached" : "interp")) continue;
            std::vector<double> mips;
            Sample last{};
            for (unsigned rep = 0; rep < repeats; ++rep) {
                last = run_once(w, iterations, cached);
                if (last.event != Event::supervisor_call) { std::fprintf(stderr, "%s: stopped with %s\n", w.name.data(), event_name(last.event)); return 1; }
                mips.push_back(last.retired / last.seconds / 1e6);
            }
            std::sort(mips.begin(), mips.end());
            if (!cached) reference = last.digest;
            else if (mode == "both" && last.digest != reference) { std::fprintf(stderr, "%s: cached digest differs from interpreter\n", w.name.data()); status = 1; }
            const char* mode = cached ? "cached" : "interp";
            if (csv) std::printf("%s,%s,%llu,%.2f,%.2f,%016llx\n", w.name.data(), mode, (unsigned long long)last.retired, mips.back(), mips[mips.size() / 2], (unsigned long long)last.digest);
            else std::printf("%-8s %-8s %12llu %10.2f %10.2f  %016llx\n", w.name.data(), mode, (unsigned long long)last.retired, mips.back(), mips[mips.size() / 2], (unsigned long long)last.digest);
        }
    }
    return status;
}
