// SPDX-License-Identifier: GPL-2.0-or-later
#include "atlan/native_functions.hpp"
#include <algorithm>
#include <sstream>

namespace atlan {
std::optional<NativeProbe::Result> NativeProbe::guest(std::uint32_t address, std::span<const std::uint32_t> arguments) {
    const auto outcome = dispatcher_.call(runtime_, address, arguments, stack_top_, return_target_, budget_);
    if (outcome.stop != Stop::returned) return std::nullopt;
    return Result{static_cast<std::uint32_t>(runtime_.cpu.registers[0]), static_cast<std::uint32_t>(runtime_.cpu.registers[1])};
}
std::optional<NativeProbe::Result> NativeProbe::native(const ImportService& service, std::span<const std::uint32_t> arguments) {
    if (!runtime_.load_arguments(arguments, stack_top_)) return std::nullopt;
    ImportCall call(dispatcher_, runtime_, "native");
    if (service(call) || call.declined() || call.blocked() || call.transferred()) return std::nullopt;
    return Result{static_cast<std::uint32_t>(runtime_.cpu.registers[0]), static_cast<std::uint32_t>(runtime_.cpu.registers[1])};
}

std::optional<std::map<std::string, NativeSignature>> parse_native_signatures(const std::string& text, std::string& why) {
    std::map<std::string, NativeSignature> out;
    std::istringstream lines(text);
    std::string line;
    for (unsigned number = 1; std::getline(lines, line); ++number) {
        if (const auto comment = line.find('#'); comment != std::string::npos) line.erase(comment);
        std::istringstream words(line);
        std::string name, mode, hex, more;
        if (!(words >> name)) continue;
        const auto bad = [&](const char* what) {
            why = "line " + std::to_string(number) + ": " + what;
            return std::nullopt;
        };
        if (!(words >> mode) || (mode != "arm" && mode != "thumb")) return bad("expected arm or thumb after the name");
        while (words >> more) hex += more;
        NativeSignature signature;
        signature.thumb = mode == "thumb";
        if (hex.empty() || hex.size() % 2 != 0) return bad("expected pairs of hex digits or ??");
        const auto digit = [](char c) { return c >= '0' && c <= '9' ? c - '0' : c >= 'a' && c <= 'f' ? c - 'a' + 10 : c >= 'A' && c <= 'F' ? c - 'A' + 10 : -1; };
        unsigned fixed = 0;
        for (std::size_t i = 0; i < hex.size(); i += 2) {
            if (hex[i] == '?' && hex[i + 1] == '?') {
                signature.bytes.push_back(0);
                signature.mask.push_back(0);
                continue;
            }
            const int high = digit(hex[i]), low = digit(hex[i + 1]);
            if (high < 0 || low < 0) return bad("expected pairs of hex digits or ??");
            signature.bytes.push_back(static_cast<std::uint8_t>(high * 16 + low));
            signature.mask.push_back(0xff);
            ++fixed;
        }
        if (fixed < 8 || signature.mask.front() == 0) return bad("a signature needs at least 8 fixed bytes and a fixed first byte");
        if (!out.emplace(name, std::move(signature)).second) return bad("name given twice");
    }
    return out;
}

namespace {
// The only address in [base, base + size) where the signature matches executable code, with the Thumb bit.
std::optional<std::uint32_t> find_signature(Memory& memory, std::uint32_t base, std::uint64_t size, const NativeSignature& signature) {
    const std::size_t length = signature.bytes.size();
    const std::uint32_t step = signature.thumb ? 2 : 4;
    std::optional<std::uint32_t> found;
    std::vector<std::uint8_t> run;  // one stretch of consecutive executable pages
    std::uint32_t run_base = 0;
    const auto scan = [&]() -> bool {  // false: a second match
        for (std::size_t at = (step - run_base % step) % step; at + length <= run.size(); at += step) {
            if (run[at] != signature.bytes[0]) continue;
            std::size_t i = 1;
            while (i < length && (run[at + i] & signature.mask[i]) == signature.bytes[i]) ++i;
            if (i != length) continue;
            if (found) return false;
            found = (run_base + static_cast<std::uint32_t>(at)) | (signature.thumb ? 1U : 0U);
        }
        return true;
    };
    for (std::uint64_t page = base & ~std::uint64_t{Memory::page_size - 1}; page < std::uint64_t{base} + size; page += Memory::page_size) {
        const auto bytes = memory.page_span(static_cast<std::uint32_t>(page), MemoryAccess::execute);
        if (bytes.empty()) {
            if (!scan()) return std::nullopt;
            run.clear();
            continue;
        }
        if (run.empty()) run_base = static_cast<std::uint32_t>(page);
        const auto* data = reinterpret_cast<const std::uint8_t*>(bytes.data());
        run.insert(run.end(), data, data + bytes.size());
    }
    if (!scan()) return std::nullopt;
    return found;
}
}

unsigned install_native_functions(ImportDispatcher& dispatcher, Runtime& runtime, std::span<const elf::Symbol> exports,
                                  std::vector<NativeSet> sets, const NativeOptions& options) {
    const auto log = [&](const std::string& line) { if (options.log) options.log(line); };
    std::map<std::string_view, std::uint32_t> functions;
    for (const auto& symbol : exports)
        if (symbol.type == 2 && symbol.address != 0) functions.emplace(symbol.name, symbol.address);
    const CpuState saved = runtime.cpu;
    runtime.memory.map(options.scratch, NativeProbe::scratch_bytes, Permissions::read_write);
    NativeProbe probe(dispatcher, runtime, options.stack_top, options.return_target, options.scratch, options.budget);
    unsigned installed = 0;
    std::string replaced, absent, rejected;
    for (auto& set : sets) {
        if (std::find(options.skip.begin(), options.skip.end(), set.name) != options.skip.end()) continue;
        std::map<std::string, std::vector<std::uint32_t>> located;  // every distinct address the names resolve to
        std::string missing;
        bool taken = false;
        for (const auto& member : set.members) {
            std::vector<std::uint32_t> addresses;
            const auto add = [&](std::uint32_t address) {
                if (dispatcher.replaces(address)) taken = true;  // an earlier set (a variant of this one) has it
                if (std::find(addresses.begin(), addresses.end(), address) == addresses.end() && !dispatcher.replaces(address) &&
                    runtime.memory.translate(address & ~1U, MemoryAccess::execute) != nullptr) addresses.push_back(address);
            };
            for (const auto& symbol : member.symbols)
                if (const auto it = functions.find(symbol); it != functions.end()) add(it->second);
            if (addresses.empty())
                if (const auto it = options.signatures.find(member.name); it != options.signatures.end())
                    if (const auto address = find_signature(runtime.memory, options.code_base, options.code_bytes, it->second)) add(*address);
            if (!addresses.empty()) located.emplace(member.name, std::move(addresses));
            else if (member.required) missing += (missing.empty() ? "" : ", ") + member.name;
        }
        if (!missing.empty()) {
            if (!taken) absent += " " + set.name;
            continue;
        }
        if (set.verify) {
            const auto member = [&](const std::string& name) -> const NativeSet::Member* {
                for (const auto& m : set.members) if (m.name == name) return &m;
                return nullptr;
            };
            const NativeSet::Call guest = [&](const std::string& name, std::span<const std::uint32_t> arguments) -> std::optional<NativeProbe::Result> {
                const auto it = located.find(name);
                if (it == located.end()) return std::nullopt;
                return probe.guest(it->second.front(), arguments);
            };
            const NativeSet::Call native = [&](const std::string& name, std::span<const std::uint32_t> arguments) -> std::optional<NativeProbe::Result> {
                const auto* m = member(name);
                if (m == nullptr) return std::nullopt;
                return probe.native(m->service, arguments);
            };
            bool agrees = false;
            try { agrees = set.verify(probe, guest, native); } catch (...) { agrees = false; }
            if (!agrees) {
                rejected += " " + set.name;
                continue;
            }
        }
        for (auto& member : set.members) {
            const auto it = located.find(member.name);
            if (it == located.end()) continue;
            bool any = false;
            for (const std::uint32_t address : it->second)
                any |= dispatcher.replace_function(runtime, address, "native:" + member.name, member.service);
            if (any) {
                ++installed;
                replaced += " " + member.name;
            }
        }
    }
    runtime.memory.unmap(options.scratch, NativeProbe::scratch_bytes);
    runtime.cpu = saved;
    // An image that has none of the functions is not worth a line.
    if (installed != 0 || !rejected.empty())
        log("native functions replaced: " + std::to_string(installed) + replaced +
            (rejected.empty() ? "" : "; guest code differs from the host implementation, left interpreted:" + rejected) +
            (absent.empty() ? "" : "; not found:" + absent));
    return installed;
}
}
