// SPDX-License-Identifier: GPL-2.0-or-later
#include "atlan/elf.hpp"
#include "atlan/runtime.hpp"
#include <charconv>
#include <fstream>
#include <iostream>
#include <limits>
#include <stdexcept>

namespace {
std::uint32_t number(std::string_view text) {
    int base = 10;
    if (text.starts_with("0x")) { text.remove_prefix(2); base = 16; }
    std::uint32_t result{};
    const auto parsed = std::from_chars(text.data(), text.data() + text.size(), result, base);
    if (text.empty() || parsed.ec != std::errc{} || parsed.ptr != text.data() + text.size())
        throw std::invalid_argument("invalid ARM32 integer argument");
    return result;
}
struct Call { std::string symbol; std::vector<std::uint32_t> args; };
}
int main(int argc, char* argv[]) {
    try {
        if (argc < 2 || std::string_view(argv[1]) == "--help") {
            std::cout << "atlan-elf <ELF32.so> [--trap-imports] [--initialize] [--bias N] [--budget N]\n"
                         "          [--call symbol [--arg N]...]...\n"
                         "Scalar calls accept at most four 32-bit register arguments.\n"
                         "Constructors run only with --initialize; unresolved imports stop execution.\n";
            return argc < 2 ? 1 : 0;
        }
        atlan::elf::Options options;
        std::vector<Call> calls;
        std::uint32_t budget = 100000;
        bool initialize = false;
        for (int i = 2; i < argc; ++i) {
            const std::string_view arg = argv[i];
            if (arg == "--trap-imports") options.unresolved = atlan::elf::UnresolvedImports::diagnostic_traps;
            else if (arg == "--initialize") initialize = true;
            else if (arg == "--bias" && i + 1 < argc) options.load_bias = number(argv[++i]);
            else if (arg == "--budget" && i + 1 < argc) budget = number(argv[++i]);
            else if (arg == "--call" && i + 1 < argc) calls.push_back({argv[++i], {}});
            else if (arg == "--arg" && i + 1 < argc && !calls.empty()) calls.back().args.push_back(number(argv[++i]));
            else throw std::invalid_argument("unknown or incomplete option");
        }
        std::ifstream input(argv[1], std::ios::binary | std::ios::ate);
        if (!input) throw std::runtime_error("cannot open input ELF");
        const auto size = input.tellg();
        if (size < 0 || size > 256 * 1024 * 1024) throw std::runtime_error("ELF input exceeds file budget");
        std::vector<std::byte> bytes(static_cast<std::size_t>(size));
        input.seekg(0);
        if (!input.read(reinterpret_cast<char*>(bytes.data()), static_cast<std::streamsize>(bytes.size())))
            throw std::runtime_error("cannot read input ELF");
        auto image = atlan::elf::load(bytes, options);
        std::cout << "loaded mapped_bytes=" << image.mapped_bytes << " exports=" << image.exports.size()
                  << " relocations=" << image.relocation_count << " dependencies=" << image.dependencies.size()
                  << " trapped_imports=" << image.trapped_imports.size()
                  << " pending_initializers=" << image.initializers.size() << '\n';
        atlan::Runtime runtime;
        runtime.memory = std::move(image.memory);
        runtime.memory.map(0x70000000, 1024 * 1024, atlan::Permissions::read_write);
        const auto invoke = [&](std::string_view label, std::uint32_t address,
                                std::span<const std::uint32_t> args) {
            const auto result = runtime.call(address, args, 0x70100000, 0xffff0000, budget);
            std::cout << label << " event=" << atlan::event_name(result.event)
                      << " r0=" << runtime.cpu.registers[0] << " retired=" << result.retired
                      << " pc=0x" << std::hex << runtime.pc() << std::dec;
            if (result.event != atlan::Event::function_return) {
                std::cout << " fault=0x" << std::hex << result.address << std::dec;
                if (const auto* import = image.import_at(result.event == atlan::Event::memory_fault ? result.address : runtime.pc()))
                    std::cout << " unresolved_import=" << import->name;
            }
            std::cout << '\n';
            return result.event == atlan::Event::function_return;
        };
        if (initialize) {
            for (std::size_t i = 0; i < image.initializers.size(); ++i)
                if (!invoke("initializer[" + std::to_string(i) + "]", image.initializers[i], {})) return 2;
            std::cout << "initializers_completed=" << image.initializers.size() << '\n';
        }
        for (const auto& call : calls) {
            const auto* symbol = image.find(call.symbol);
            if (!symbol || symbol->type != 2) throw std::invalid_argument("function export not found: " + call.symbol);
            if (!invoke(call.symbol, symbol->address, call.args)) return 2;
        }
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
