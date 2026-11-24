// SPDX-License-Identifier: GPL-2.0-or-later
#include "atlan/abi.hpp"
#include "atlan/android_log.hpp"
#include "atlan/elf.hpp"
#include "atlan/files.hpp"
#include "atlan/heap.hpp"
#include "atlan/jni.hpp"
#include "atlan/libc_services.hpp"
#include "atlan/mmap.hpp"
#include "atlan/opensles.hpp"
#include "atlan/stdio.hpp"
#include "atlan/string_services.hpp"
#include "atlan/lifetime.hpp"
#include "atlan/runtime.hpp"
#include "atlan/threads.hpp"
#include <array>
#include <charconv>
#include <random>
#include <fstream>
#include <iostream>
#include <limits>
#include <optional>
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
struct Call { std::string symbol; std::vector<std::uint32_t> args; std::vector<std::pair<std::size_t, std::string>> strings; };
}
int main(int argc, char* argv[]) {
    try {
        if (argc < 2 || std::string_view(argv[1]) == "--help") {
            std::cout << "atlan-elf <ELF32.so> [--trap-imports] [--initialize] [--bias N] [--budget N]\n"
                         "          [--log] [--jni] [--root DIR] [--pump N] [--call symbol [--arg N]...]...\n"
                         "Calls take up to 32 word arguments (extras go on the guest stack).\n"
                         "--log prints __android_log_* output; --jni maps a minimal JavaVM/JNIEnv and\n"
                         "passes the JavaVM pointer as the first argument of a JNI_OnLoad call.\n"
                         "Guest threads are cooperative: they run while a call blocks or exceeds its time slice.\n"
                         "--pump N runs the remaining ready guest threads for up to N instructions after the calls.\n"
                         "Constructors run only with --initialize; unresolved imports stop execution.\n";
            return argc < 2 ? 1 : 0;
        }
        atlan::elf::Options options;
        std::vector<Call> calls;
        std::uint32_t budget = 100000, pump_budget = 0;
        bool initialize = false, log = false, jni = false;
        std::string root;
        for (int i = 2; i < argc; ++i) {
            const std::string_view arg = argv[i];
            if (arg == "--trap-imports") options.unresolved = atlan::elf::UnresolvedImports::diagnostic_traps;
            else if (arg == "--initialize") initialize = true;
            else if (arg == "--log") log = true;
            else if (arg == "--root" && i + 1 < argc) root = argv[++i];
            else if (arg == "--jni") jni = true;
            else if (arg == "--pump" && i + 1 < argc) pump_budget = number(argv[++i]);
            else if (arg == "--bias" && i + 1 < argc) options.load_bias = number(argv[++i]);
            else if (arg == "--budget" && i + 1 < argc) budget = number(argv[++i]);
            else if (arg == "--call" && i + 1 < argc) calls.push_back({argv[++i], {}, {}});
            else if (arg == "--str" && i + 1 < argc && !calls.empty()) {
                calls.back().strings.emplace_back(calls.back().args.size(), argv[++i]);
                calls.back().args.push_back(0);  // replaced by a jstring handle
            }
            else if (arg == "--arg" && i + 1 < argc && !calls.empty()) calls.back().args.push_back(number(argv[++i]));
            else throw std::invalid_argument("unknown or incomplete option");
        }
        // __stack_chk_guard is a data import: the host provides one guest word, never a trap.
        constexpr std::uint32_t guard_page = 0x6ffff000;
        std::random_device entropy;
        const std::uint32_t canary = entropy() & 0xffffff00U;
        options.resolve = [&](std::string_view name, std::uint8_t) -> std::optional<std::uint32_t> {
            if (name == "__stack_chk_guard") return guard_page;
            // Bionic's stdin/stdout/stderr FILE array (3 x 84 bytes); stdio services identify streams by address.
            if (name == "__sF") return guard_page + 0x100;
            if (const auto iid = atlan::OpenSles::data_symbol(name)) return *iid;
            return std::nullopt;
        };
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
        atlan::ImportDispatcher dispatcher;
        atlan::ObjectLifetime lifetime;
        lifetime.install(dispatcher);
        atlan::install_libc_services(dispatcher);
        atlan::GuestHeap heap;
        atlan::install_heap_services(dispatcher, heap);
        atlan::install_string_services(dispatcher);
        atlan::install_printf_services(dispatcher);
        std::optional<atlan::GuestStdio> stdio;
        if (!root.empty()) {
            stdio.emplace(root, guard_page + 0x200, [&](const std::string& path, const std::string& mode, bool ok) {
                if (log) std::cout << "[fopen] " << path << " mode=" << mode << (ok ? " ok" : " failed") << '\n';
            });
            stdio->install(dispatcher);
        }
        const std::map<std::string, std::string> properties;
        atlan::install_system_properties(dispatcher, properties, [&](const std::string& name, bool known) {
            if (log) std::cout << "[property] " << name << (known ? "" : " (unset)") << '\n';
        });
        atlan::GuestMmap mmap_arena;
        mmap_arena.install(dispatcher);
        atlan::GuestThreads::Config thread_config;
        thread_config.errno_address = guard_page + 0x200;
        atlan::GuestThreads threads(thread_config);
        threads.install(dispatcher);
        atlan::MutexTable mutexes(&threads);
        mutexes.install(dispatcher);
        atlan::CondTable conds(threads, mutexes);
        conds.install(dispatcher);
        atlan::VirtualFiles files(guard_page + 0x200);
        files.install(dispatcher);
        atlan::SemaphoreTable semaphores(&threads);
        semaphores.install(dispatcher);
        atlan::ThreadKeys thread_keys(&threads);
        thread_keys.install(dispatcher);
        atlan::AndroidLog android_log([&](const atlan::LogEntry& entry) {
            if (log) std::cout << "[android_log " << entry.priority << "] " << entry.tag << ": " << entry.message << '\n';
        });
        android_log.install(dispatcher);
        dispatcher.attach(image);
        runtime.memory = std::move(image.memory);
        runtime.memory.map(guard_page, atlan::Memory::page_size, atlan::Permissions::read_write);
        {
            std::array<std::byte, 4> bytes{};
            for (unsigned i = 0; i < 4; ++i) bytes[i] = std::byte((canary >> (8 * i)) & 255);
            if (!runtime.memory.write(guard_page, bytes)) throw std::runtime_error("cannot initialize stack guard");
        }
        runtime.memory.map(0x70000000, 1024 * 1024, atlan::Permissions::read_write);
        std::optional<atlan::JavaBootstrap> java;
        if (jni) java.emplace(runtime, dispatcher, atlan::JavaBootstrap::Config{});
        atlan::RecordingAudioSink audio_sink(true);  // headless: consumes PCM as soon as it plays
        std::optional<atlan::OpenSles> audio;
        {
            atlan::OpenSles::Config audio_config;
            audio_config.sink = &audio_sink;
            audio_config.trace = [&](const std::string& line) { if (log) std::cout << "[opensles] " << line << '\n'; };
            audio.emplace(runtime, dispatcher, audio_config);
        }
        const auto invoke = [&](std::string_view label, std::uint32_t address,
                                std::span<const std::uint32_t> args) {
            const auto outcome = dispatcher.call(runtime, address, args, 0x70100000, 0xffff0000, budget);
            const auto& result = outcome.cpu;
            std::cout << label << " event=" << atlan::event_name(result.event)
                      << " r0=" << runtime.cpu.registers[0] << " retired=" << result.retired
                      << " pc=0x" << std::hex << runtime.pc() << std::dec;
            if (!outcome.message.empty()) std::cout << " service_error=\"" << outcome.message << '"';
            if (outcome.stop == atlan::Stop::unresolved_import) std::cout << " missing_service=" << outcome.import << " args=" << runtime.cpu.registers[0] << "," << runtime.cpu.registers[1] << "," << runtime.cpu.registers[2];
            if (outcome.stop == atlan::Stop::service_error) std::cout << " failing_service=" << outcome.import;
            if (outcome.stop == atlan::Stop::deadlock) std::cout << " deadlock";
            if (outcome.thread > 1) std::cout << " guest_thread=" << outcome.thread;
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
            auto args = call.args;
            if (java && call.symbol == "JNI_OnLoad") args = {java->vm(), 0};
            else if (java && call.symbol.starts_with("Java_")) {
                // Java_<pkg>_<Class>_<method>: pass env and a host object of that class as `this`.
                const auto cut = call.symbol.rfind('_');
                std::string cls = call.symbol.substr(5, cut - 5);
                for (auto& ch : cls) if (ch == '_') ch = '/';
                for (const auto& [index, text] : call.strings) args[index] = java->new_string(text);
                args.insert(args.begin(), {java->env(), java->new_object(java->class_handle(cls))});
            }
            const bool ok = invoke(call.symbol, symbol->address, args);
            if (java) {
                for (const auto& cls : java->classes()) std::cout << "jni class " << cls << '\n';
                for (const auto& m : java->members())
                    std::cout << "jni member kind=" << static_cast<int>(m.kind) << ' ' << m.class_name << '.' << m.name << ' ' << m.signature << '\n';
                for (const auto& m : java->native_methods())
                    std::cout << "jni native " << m.class_name << '.' << m.name << m.signature << " -> 0x" << std::hex << m.address << std::dec << '\n';
            }
            if (!ok) return 2;
        }
        const auto print_string_stats = [&] {
            if (!java) return;
            const auto& st = java->string_input_stats();
            std::cout << "jni NewStringUTF input: calls=" << st.calls << " ascii=" << st.ascii << " utf8=" << st.utf8
                      << " cp949_only=" << st.cp949_only << " neither=" << st.neither << '\n';
        };
        print_string_stats();
        if (pump_budget != 0) {
            const auto outcome = dispatcher.pump(runtime, pump_budget);
            std::cout << "pump stop=" << static_cast<int>(outcome.stop) << " retired=" << outcome.retired
                      << " live_threads=" << threads.live_threads();
            if (!outcome.message.empty()) std::cout << " message=\"" << outcome.message << '"';
            if (outcome.stop == atlan::Stop::unresolved_import) std::cout << " missing_service=" << outcome.import;
            if (outcome.stop == atlan::Stop::service_error) std::cout << " failing_service=" << outcome.import;
            if (outcome.thread != 0) std::cout << " guest_thread=" << outcome.thread << " pc=0x" << std::hex << runtime.pc() << std::dec;
            std::cout << '\n';
            print_string_stats();
            if (outcome.stop != atlan::Stop::returned) return 2;
        }
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
