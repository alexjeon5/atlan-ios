// SPDX-License-Identifier: GPL-2.0-or-later
#include "shell/session.hpp"
#include "shell/app_adapter.hpp"
#include "shell/sha256.hpp"
#include <iterator>
#include <chrono>
#include <algorithm>
#include "atlan/gles.hpp"
#include "atlan/elf.hpp"
#include "atlan/android_log.hpp"
#include "atlan/files.hpp"
#include "atlan/heap.hpp"
#include "atlan/jni.hpp"
#include "atlan/abi.hpp"
#include "atlan/data_symbols.hpp"
#include "atlan/stdio.hpp"
#include "atlan/libc_services.hpp"
#include "atlan/libc_extra.hpp"
#include "atlan/mmap.hpp"
#include "atlan/native_functions.hpp"
#include "atlan/opensles.hpp"
#include "atlan/profiler.hpp"
#include "atlan/string_services.hpp"
#include "atlan/lifetime.hpp"
#include "atlan/runtime.hpp"
#include "atlan/scanf.hpp"
#include "atlan/sockets.hpp"
#include "atlan/system_services.hpp"
#include "atlan/threads.hpp"
#include <array>
#include <chrono>
#include <exception>
#include <map>
#include <optional>
#include <random>
#include <fstream>
#include <pthread.h>
#include <set>
#include <stdexcept>
#include <thread>
#include <vector>

namespace atlan::shell {
struct GuestSession::Thread {
    pthread_t handle{};
    bool joinable{};
};

namespace {
constexpr std::uint64_t max_library_bytes = 256ull * 1024 * 1024;
constexpr std::uint32_t stack_base = 0x70000000;
constexpr std::uint32_t stack_bytes = 1024 * 1024;
constexpr std::uint32_t return_sentinel = 0xffff0000;
constexpr std::uint32_t guard_page = 0x6ffff000;  // stack guard word, __sF, virtual file table

std::vector<std::byte> read_library(const std::string& path) {
    std::ifstream input(path, std::ios::binary | std::ios::ate);
    if (!input) throw std::runtime_error("cannot open guest library");
    const auto size = input.tellg();
    if (size <= 0 || static_cast<std::uint64_t>(size) > max_library_bytes)
        throw std::runtime_error("guest library size is out of range");
    std::vector<std::byte> bytes(static_cast<std::size_t>(size));
    input.seekg(0);
    if (!input.read(reinterpret_cast<char*>(bytes.data()), static_cast<std::streamsize>(bytes.size())))
        throw std::runtime_error("cannot read guest library");
    return bytes;
}
}

namespace {
std::string hex(std::uint64_t value) {
    static constexpr char digits[] = "0123456789abcdef";
    std::string out;
    do { out.insert(out.begin(), digits[value & 15]); value >>= 4; } while (value);
    return out;
}
// `image`, when given, names a data import nobody provides: the guest reads it at its unmapped trap address.
std::string describe_stop(const std::string& label, const CallOutcome& outcome, const elf::Image* image = nullptr) {
    std::string line = label + " stopped after " + std::to_string(outcome.retired) + " instructions";
    const auto* data = image && outcome.stop == Stop::cpu_event && outcome.cpu.event == Event::memory_fault ? image->import_at(outcome.cpu.address) : nullptr;
    if (outcome.stop == Stop::unresolved_import) line += " missing_service=" + outcome.import;
    else if (data) line += " missing_data=" + data->name;
    else if (outcome.stop == Stop::service_error) line += " failing_service=" + outcome.import + " " + outcome.message;
    else if (outcome.stop == Stop::budget_exhausted) line += " budget_exhausted";
    else line += std::string(" event=") + event_name(outcome.cpu.event) + " address=0x" + hex(outcome.cpu.address) +
                 " access=" + std::to_string(static_cast<int>(outcome.cpu.access));
    return line;
}
// The JNI export name of a native method (Java_<class>_<method>, '_' escaped as "_1").
std::string jni_symbol(std::string_view class_name, std::string_view name) {
    std::string symbol = "Java_";
    for (char c : class_name) { if (c == '/') symbol += '_'; else if (c == '_') symbol += "_1"; else symbol += c; }
    symbol += '_';
    for (char c : name) { if (c == '_') symbol += "_1"; else symbol += c; }
    return symbol;
}
// The method part of a JNI export name, for log lines.
std::string method_label(std::string_view symbol, std::string_view fallback) {
    if (symbol.empty()) return std::string(fallback);
    return std::string(symbol.substr(symbol.rfind('_') + 1));
}
std::string describe_state(Runtime& runtime, const ImportDispatcher& dispatcher) {
    std::string out = "state pc=0x" + hex(runtime.pc()) + " lr=0x" + hex(runtime.cpu.registers[14]) +
                      " sp=0x" + hex(runtime.cpu.registers[13]);
    for (unsigned i = 0; i < 4; ++i) out += " r" + std::to_string(i) + "=0x" + hex(runtime.cpu.registers[i]);
    std::vector<std::pair<std::string, std::uint64_t>> top(dispatcher.import_counts().begin(), dispatcher.import_counts().end());
    std::sort(top.begin(), top.end(), [](const auto& a, const auto& b) { return a.second > b.second; });
    out += " stack:";
    for (std::uint32_t i = 0; i < 24; ++i) {  // enough to find the callers' return addresses
        std::array<std::byte, 4> word{};
        if (!runtime.memory.read(static_cast<std::uint32_t>(runtime.cpu.registers[13]) + 4 * i, word)) break;
        std::uint32_t value = 0;
        for (unsigned b = 0; b < 4; ++b) value |= std::to_integer<std::uint32_t>(word[b]) << (8 * b);
        out += " " + hex(value);
    }
    out += " imports:";
    for (std::size_t i = 0; i < top.size() && i < 8; ++i) out += " " + top[i].first + "=" + std::to_string(top[i].second);
    return out;
}
}

GuestSession::GuestSession(SessionConfig config, Log log)
    : config_(std::move(config)), log_(std::move(log)) {}
GuestSession::~GuestSession() { stop(); join(); }

bool GuestSession::start() {
    if (thread_) return false;
    auto holder = std::make_unique<Thread>();
    pthread_attr_t attributes;
    pthread_attr_init(&attributes);
    pthread_attr_setstacksize(&attributes, 8u * 1024 * 1024);
    state_.store(SessionState::running);
    const int rc = pthread_create(&holder->handle, &attributes, [](void* self) -> void* {
        static_cast<GuestSession*>(self)->run();
        return nullptr;
    }, this);
    pthread_attr_destroy(&attributes);
    if (rc != 0) { state_.store(SessionState::failed); return false; }
    holder->joinable = true;
    thread_ = std::move(holder);
    return true;
}

void GuestSession::join() noexcept {
    if (thread_ && thread_->joinable) { pthread_join(thread_->handle, nullptr); thread_->joinable = false; }
}

void GuestSession::run() {
    auto fail = [this](const std::string& why) { log_(why); state_.store(SessionState::failed); };
    AppAdapter bare_adapter;  // no app: a bare ELF image, its constructors and JNI_OnLoad
    AppAdapter& adapter = config_.adapter ? *config_.adapter : bare_adapter;
    try {
        elf::Options options;
        options.unresolved = elf::UnresolvedImports::diagnostic_traps;
        options.resolve = [](std::string_view name, std::uint8_t) -> std::optional<std::uint32_t> {
            if (name == "__stack_chk_guard") return guard_page;
            if (name == "__sF") return guard_page + 0x100;
            if (const auto iid = OpenSles::data_symbol(name)) return *iid;
            if (const auto data = BionicData::symbol(name)) return *data;
            return std::nullopt;
        };
        const std::string library_path = config_.library_path.empty() ? adapter.find_library(config_.data_root) : config_.library_path;
        if (library_path.empty()) throw std::runtime_error(adapter.missing_library());
        auto library = read_library(library_path);
        adapter.unpack_library(library, log_);
        log_("library sha256=" + sha256_hex(library));  // identifies the build (a list of verified builds is keyed by it)
        auto image = elf::load(library, options);
        log_("loaded mapped_bytes=" + std::to_string(image.mapped_bytes) +
             " exports=" + std::to_string(image.exports.size()) +
             " imports_trapped=" + std::to_string(image.trapped_imports.size()) +
             " initializers=" + std::to_string(image.initializers.size()));
        const auto stopped = [&image](const std::string& label, const CallOutcome& outcome) { return describe_stop(label, outcome, &image); };
        Runtime runtime;
        ImportDispatcher dispatcher;
        dispatcher.set_block_cache(config_.block_cache);
        // Per-function profile, written per phase; a phase ends where write_profile() is called.
        std::optional<GuestProfiler> profiler;
        std::map<std::string, unsigned> profile_phases;
        if (!config_.profile_path.empty()) {
            profiler.emplace();
            for (const auto& symbol : image.exports)
                if (symbol.type == 2) profiler->add_function(symbol.address, symbol.name);
            for (const auto& import : image.trapped_imports) profiler->add_function(import.address, "import:" + import.name);
            dispatcher.set_profiler(&*profiler);
        }
        const auto write_profile = [&](const std::string& phase) {
            if (!profiler) return;
            const unsigned n = profile_phases[phase]++;
            const std::string path = config_.profile_path + "." + phase + (n ? "-" + std::to_string(n) : "") + ".tsv";
            if (std::ofstream out(path); out) profiler->write(out);
            log_("profile " + path + ": " + profiler->summary(12));
            profiler->reset();
        };
        dispatcher.set_import_timing(config_.perf_log);
        ObjectLifetime lifetime;
        lifetime.install(dispatcher);
        install_libc_services(dispatcher);
        install_time_services(dispatcher, guard_page + 0x400);
        install_libc_extra(dispatcher);
        install_scanf_services(dispatcher);
        install_system_services(dispatcher, guard_page + 0x200);
        GuestHeap heap;
        install_heap_services(dispatcher, heap);
        install_string_services(dispatcher);
        install_printf_services(dispatcher);
        install_console_services(dispatcher, guard_page + 0x100, guard_page + 0x200, [this, seen = std::set<std::string>()](const std::string& text) mutable {
            if (!seen.insert(text).second) return;  // the guest repeats its polling errors; each text is logged once
            log_("guest stdout " + text);
        });
        const std::map<std::string, std::string> properties;
        install_system_properties(dispatcher, properties, [this](const std::string& name, bool known) {
            log_("property " + name + (known ? "" : " (unset)"));
        });
        std::optional<GuestStdio> stdio;
        if (!config_.data_root.empty()) {
            // The guest polls some files that never exist; a failed open is logged the first time only.
            stdio.emplace(config_.data_root, guard_page + 0x200, [this, failed = std::set<std::string>()](const std::string& path, const std::string& mode, bool ok) mutable {
                if (!ok && !failed.insert(path + '\n' + mode).second) return;
                log_("fopen " + path + " mode=" + mode + (ok ? " ok" : " failed"));
            });
            adapter.configure_files(*stdio, log_);  // the app's folder layout and settings
            stdio->install(dispatcher);
        }
        GuestMmap mmap_arena;
        if (stdio) mmap_arena.set_host_descriptors([&](std::uint32_t fd) { return stdio->host_descriptor(fd); });
        mmap_arena.install(dispatcher);
        GuestThreads::Config thread_config;
        thread_config.errno_address = guard_page + 0x200;
        GuestThreads threads(thread_config);
        threads.install(dispatcher);
        MutexTable mutexes(&threads);
        mutexes.install(dispatcher);
        CondTable conds(threads, mutexes);
        conds.install(dispatcher);
        VirtualFiles files(guard_page + 0x200);
        if (stdio) files.set_regular_file_size([&](std::uint32_t fd) { return stdio->descriptor_size(fd); });
        files.install(dispatcher);
        // Host sockets for the guest's own network code; each connection is logged (addresses and sizes only).
        GuestSockets::Config socket_config;
        socket_config.errno_address = guard_page + 0x200;
        socket_config.threads = &threads;
        socket_config.trace = [this](const std::string& line) { log_(line); };
        GuestSockets sockets(socket_config);
        sockets.install(dispatcher);
        SemaphoreTable semaphores(&threads);
        semaphores.install(dispatcher);
        ThreadKeys thread_keys(&threads);
        thread_keys.install(dispatcher);
        AndroidLog android_log([this](const LogEntry& entry) {
            log_("android_log " + std::to_string(entry.priority) + " " + entry.tag + ": " + entry.message);
        });
        android_log.install(dispatcher);
        NullGlBackend null_gl;
        Gles2 gles(config_.gl_backend ? *config_.gl_backend : static_cast<GlBackend&>(null_gl), Gles2::Config{guard_page + 0x800});
        gles.install(dispatcher);
        dispatcher.attach(image);
        auto initializers = std::move(image.initializers);
        const auto* on_load = image.find("JNI_OnLoad");
        const std::uint32_t on_load_address = on_load && on_load->type == 2 ? on_load->address : 0;
        const AppStartup startup = adapter.startup();
        const auto* native_init = startup.init.empty() ? nullptr : image.find(startup.init);
        const std::uint32_t native_init_address = native_init && native_init->type == 2 ? native_init->address : 0;
        runtime.memory = std::move(image.memory);
        runtime.memory.map(guard_page, Memory::page_size, Permissions::read_write);
        {
            std::random_device entropy;
            const std::uint32_t canary = entropy() & 0xffffff00U;
            std::array<std::byte, 4> word{};
            for (unsigned i = 0; i < 4; ++i) word[i] = std::byte((canary >> (8 * i)) & 255);
            if (!runtime.memory.write(guard_page, word)) throw std::runtime_error("cannot initialize stack guard");
        }
        runtime.memory.map(stack_base, stack_bytes, Permissions::read_write);
        BionicData::map(runtime.memory);
        RecordingAudioSink fallback_sink(true);  // used when the embedder supplies no device sink
        AudioSink& audio_sink = config_.audio_sink ? *config_.audio_sink : fallback_sink;
        std::optional<OpenSles> audio;
        {
            OpenSles::Config audio_config;
            audio_config.sink = &audio_sink;
            audio_config.trace = [this](const std::string& line) { log_("opensles " + line); };
            audio.emplace(runtime, dispatcher, audio_config);
        }
        // Every host service is bound now. An import still without one is listed before anything runs: a call of it
        // stops there by name (missing_service), a read of a data import stops at its trap address (missing_data).
        // A new build of the app that needs more of the runtime shows up here instead of somewhere in the middle.
        {
            std::size_t missing = 0;
            for (const auto& import : image.trapped_imports) {
                const bool data = import.type == 1;  // STT_OBJECT; everything else is called (ImportDispatcher::attach)
                if (!data && dispatcher.service(import.name)) continue;
                log_(std::string(data ? "compat missing_data " : "compat missing_import ") + import.name);
                ++missing;
            }
            log_("compat imports trapped=" + std::to_string(image.trapped_imports.size()) + " missing=" + std::to_string(missing));
        }
        // A thread that waits on its own mutex for a buffer-queue callback to unlock it (synchronous sound playback):
        // deliver due callbacks while it waits, and keep waiting while the sink still has buffers to play.
        mutexes.set_idle_wait([&audio, this](ImportCall& call) {
            const auto delivered = audio->pump_nested(call.runtime());
            if (delivered.error) { log_("audio callback: " + *delivered.error); return MutexTable::Idle::none; }
            if (delivered.callbacks != 0) return MutexTable::Idle::progressed;
            return delivered.pending != 0 ? MutexTable::Idle::waiting : MutexTable::Idle::none;
        });
        if (config_.native_functions) {
            NativeOptions native_options;
            native_options.stack_top = stack_base + stack_bytes;
            native_options.return_target = return_sentinel;
            native_options.code_base = image.load_bias;
            native_options.code_bytes = image.mapped_bytes;
            native_options.log = log_;
            native_options.skip = config_.native_skip;
            if (std::ifstream file(config_.native_signatures); file) {
                std::string why;
                const std::string text((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());
                if (auto parsed = parse_native_signatures(text, why)) native_options.signatures = std::move(*parsed);
                else log_("native signatures rejected: " + why);
            }
            auto sets = runtime_library_sets();
            for (auto& set : image_library_sets()) sets.push_back(std::move(set));
            for (auto& set : adapter.native_sets()) sets.push_back(std::move(set));
            const auto native_start = std::chrono::steady_clock::now();
            install_native_functions(dispatcher, runtime, image.exports, std::move(sets), native_options);
            if (config_.perf_log)
                log_("perf native function install: " +
                     std::to_string(std::chrono::duration<double>(std::chrono::steady_clock::now() - native_start).count()) + " s");
            dispatcher.reset_import_counts();
            if (profiler) profiler->reset();
        }
        for (std::size_t i = 0; i < initializers.size(); ++i) {
            if (stop_.load()) { log_("stopped before initializer " + std::to_string(i)); state_.store(SessionState::finished); return; }
            const auto outcome = dispatcher.call(runtime, initializers[i], {}, stack_base + stack_bytes,
                                                 return_sentinel, config_.initializer_budget);
            if (outcome.stop != Stop::returned) { fail(stopped("initializer[" + std::to_string(i) + "]", outcome)); return; }
        }
        log_("initializers_completed=" + std::to_string(initializers.size()));
        if (on_load_address != 0 && !stop_.load()) {
            JavaBootstrap java(runtime, dispatcher, JavaBootstrap::Config{});
            // What the adapter may use while the guest runs: guest calls with this session's stop reporting.
            struct Host final : AppHost {
                GuestSession& session;
                Runtime& runtime;
                ImportDispatcher& dispatcher;
                JavaBootstrap& bootstrap;
                const elf::Image& image;
                const std::function<void(const std::string&)>& fail;
                AudioSink& sink;
                Host(GuestSession& s, Runtime& r, ImportDispatcher& d, JavaBootstrap& j, const elf::Image& i,
                     const std::function<void(const std::string&)>& f, AudioSink& a)
                    : session(s), runtime(r), dispatcher(d), bootstrap(j), image(i), fail(f), sink(a) {}
                JavaBootstrap& java() override { return bootstrap; }
                AudioSink& audio() override { return sink; }
                std::uint32_t function(std::string_view symbol) const override {
                    const auto* sym = image.find(symbol);
                    return sym && sym->type == 2 ? sym->address : 0;
                }
                std::uint32_t native_method(std::string_view cls, std::string_view name, std::string_view signature) const override {
                    for (const auto& m : bootstrap.native_methods())
                        if (m.class_name == cls && m.name == name && m.signature == signature) return m.address;
                    return function(jni_symbol(cls, name));
                }
                std::optional<std::uint32_t> call(const std::string& label, std::uint32_t address, std::span<const std::uint32_t> words) override {
                    const auto r = dispatcher.call(runtime, address, words, stack_base + stack_bytes, return_sentinel,
                                                   session.config_.surface_call_budget);
                    if (r.stop != Stop::returned) {
                        session.log_(describe_state(runtime, dispatcher));
                        fail(describe_stop(label, r, &image));
                        return std::nullopt;
                    }
                    return static_cast<std::uint32_t>(runtime.cpu.registers[0]);
                }
                std::optional<std::uint32_t> call_nested(std::uint32_t address, std::span<const std::uint32_t> words, std::string& message) override {
                    const auto outcome = dispatcher.call_back(runtime, address, words);
                    if (outcome.stop != Stop::returned) { message = outcome.message; return std::nullopt; }
                    return static_cast<std::uint32_t>(runtime.cpu.registers[0]);
                }
                const SurfaceState* surface() const override { return session.config_.surface; }
                const SessionLog& log() const override { return session.log_; }
            };
            const std::function<void(const std::string&)> fail_session = fail;
            Host host(*this, runtime, dispatcher, java, image, fail_session, audio_sink);
            adapter.install_java(host, config_.render_on_demand);
            struct Uninstall {
                AppAdapter& adapter;
                ~Uninstall() { adapter.uninstall_java(); }
            } uninstall{adapter};
            const bool on_demand = adapter.frames_on_request();
            const std::array<std::uint32_t, 2> args{java.vm(), 0};
            const auto outcome = dispatcher.call(runtime, on_load_address, args, stack_base + stack_bytes,
                                                 return_sentinel, config_.initializer_budget);
            for (const auto& cls : java.classes()) log_("jni class " + cls);
            for (const auto& m : java.native_methods()) log_("jni native " + m.class_name + "." + m.name + m.signature);
            // Java methods the guest looked up that nobody answers (calling one stops by name), each listed once.
            std::set<std::string> reported_methods;
            const auto report_java = [&] {
                for (const auto& m : java.unhandled_methods())
                    if (const auto line = m.class_name + "." + m.name + m.signature; reported_methods.insert(line).second)
                        log_("compat jni_unhandled " + line);
            };
            report_java();
            if (outcome.stop != Stop::returned) { fail(stopped("JNI_OnLoad", outcome)); return; }
            log_("JNI_OnLoad returned 0x" + hex(runtime.cpu.registers[0]));
            // The adapter's entry points and the natives its input bindings call: a build that renamed or dropped one
            // is listed now (the session would otherwise only note it where the call is due).
            {
                std::size_t missing = 0;
                std::vector<std::string> entries;
                if (!startup.init.empty()) entries.push_back(startup.init);
                for (const auto& call : startup.surface_calls) entries.push_back(call.symbol);
                for (const auto* entry : {&startup.render, &startup.exit_poll}) if (!entry->empty()) entries.push_back(*entry);
                for (const auto& entry : entries)
                    if (host.function(entry) == 0) { log_("compat missing_entry " + entry); ++missing; }
                std::vector<const NativeBinding*> bindings;
                for (const auto& touch : config_.input_bindings.touch) if (touch) bindings.push_back(&*touch);
                for (const auto& location : config_.input_bindings.location) bindings.push_back(&location);
                std::set<std::string> seen;
                for (const auto* b : bindings) {
                    const auto line = b->class_name + "." + b->name + b->signature;
                    if (!seen.insert(line).second || host.native_method(b->class_name, b->name, b->signature) != 0) continue;
                    log_("compat missing_native " + line);
                    ++missing;
                }
                log_("compat entries=" + std::to_string(entries.size() + seen.size()) + " missing=" + std::to_string(missing));
            }
            // Block cache coverage since the start and the busiest imports since the last reset_import_counts().
            const auto perf_details = [&]() -> std::string {
                std::string line;
                if (const auto* runner = dispatcher.block_runner()) {
                    const auto& st = runner->stats();
                    line += "; blocks built " + std::to_string(st.blocks_built) + ", flushes " + std::to_string(st.flushes) +
                            ", without fast handler " + std::to_string(st.generic) + ", fallbacks " + std::to_string(st.fallbacks) +
                            ", single steps " + std::to_string(st.slow_steps) + "; top forms without fast handler:";
                    const auto forms = runner->generic_forms();
                    for (std::size_t i = 0; i < forms.size() && i < 24; ++i) line += " " + forms[i].first + "=" + std::to_string(forms[i].second);
                }
                std::vector<std::pair<std::string, std::uint64_t>> top(dispatcher.import_counts().begin(), dispatcher.import_counts().end());
                std::sort(top.begin(), top.end(), [](const auto& a, const auto& b) { return a.second > b.second; });
                line += "; top imports:";
                for (std::size_t i = 0; i < top.size() && i < 16; ++i) line += " " + top[i].first + "=" + std::to_string(top[i].second);
                // Host time inside import services as ms/calls (a service that calls back into the guest includes that code).
                std::vector<std::pair<std::string, std::uint64_t>> slow(dispatcher.import_nanoseconds().begin(), dispatcher.import_nanoseconds().end());
                std::sort(slow.begin(), slow.end(), [](const auto& a, const auto& b) { return a.second > b.second; });
                std::uint64_t total = 0;
                for (const auto& entry : slow) total += entry.second;
                line += "; import time " + std::to_string(static_cast<double>(total) / 1e6) + " ms:";
                for (std::size_t i = 0; i < slow.size() && i < 16 && slow[i].second != 0; ++i)
                    line += " " + slow[i].first + "=" + std::to_string(static_cast<double>(slow[i].second) / 1e6) + "/" +
                            std::to_string(dispatcher.import_counts().count(slow[i].first) ? dispatcher.import_counts().at(slow[i].first) : 0);
                return line;
            };
            if (native_init_address != 0) {
                const std::array<std::uint32_t, 2> init_args{java.env(), java.new_object(java.class_handle(startup.receiver_class))};
                dispatcher.reset_import_counts();
                const auto init_start = std::chrono::steady_clock::now();
                const auto started = dispatcher.call(runtime, native_init_address, init_args, stack_base + stack_bytes,
                                                     return_sentinel, config_.native_init_budget);
                if (config_.perf_log) {
                    const double seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - init_start).count();
                    log_("perf " + method_label(startup.init, "init") + ": " + std::to_string(started.retired) + " instructions, " +
                         std::to_string(seconds) + " s" + perf_details());
                }
                report_java();
                if (started.stop != Stop::returned) {
                    if (started.stop == Stop::budget_exhausted) log_(describe_state(runtime, dispatcher));
                    fail(stopped(method_label(startup.init, "init"), started));
                    return;
                }
                log_(method_label(startup.init, "init") + " returned 0x" + hex(runtime.cpu.registers[0]));
            } else {
                log_(method_label(startup.init, "init") + " not exported");
            }
            // Surface and render entry points (plain Java_ exports: (env, this[, surface]) -> int).
            const auto native = [&](const std::string& name, std::vector<std::uint32_t> extra) -> bool {
                const auto* sym = image.find(name);
                if (!sym || sym->type != 2) { log_(name + " not exported"); return false; }
                std::vector<std::uint32_t> call_args{java.env(), java.new_object(java.class_handle(startup.receiver_class))};
                call_args.insert(call_args.end(), extra.begin(), extra.end());
                dispatcher.reset_import_counts();
                const auto perf_start = std::chrono::steady_clock::now();
                const auto r = dispatcher.call(runtime, sym->address, call_args, stack_base + stack_bytes,
                                               return_sentinel, config_.surface_call_budget);
                if (config_.perf_log) {
                    const double seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - perf_start).count();
                    log_(std::string("perf ") + name + ": " + std::to_string(r.retired) + " instructions, " + std::to_string(seconds) +
                         " s, " + std::to_string(seconds > 0 ? r.retired / seconds / 1e6 : 0.0) + " MIPS" + perf_details());
                }
                if (const std::string_view full(name); full != startup.render)  // the first frames go into one file
                    write_profile(std::string(full.substr(full.rfind('_') + 1)));
                if (r.stop != Stop::returned) {
                    log_(describe_state(runtime, dispatcher));  // where it stopped: pc, lr, sp, r0-r3, busiest imports
                    fail(stopped(name, r));
                    return false;
                }
                log_(std::string(name) + " returned 0x" + hex(runtime.cpu.registers[0]));
                return true;
            };
            // Host input -> guest: each queued event becomes one call of the native method bound to it.
            struct BoundNative { std::uint32_t address{}, receiver{}; };
            std::map<const NativeBinding*, BoundNative> bound;
            std::map<std::pair<const NativeBinding*, std::size_t>, std::uint32_t> text_arguments;  // reused String objects
            std::set<std::string> input_notes;
            LocationWatch location_watch;
            const auto note = [&](const std::string& line) { if (input_notes.insert(line).second) log_(line); };
            const auto resolve = [&](const NativeBinding& b) -> BoundNative {
                if (const auto it = bound.find(&b); it != bound.end()) return it->second;
                BoundNative out;
                for (const auto& m : java.native_methods())
                    if (m.class_name == b.class_name && m.name == b.name && m.signature == b.signature) out.address = m.address;
                if (out.address == 0) out.address = host.function(jni_symbol(b.class_name, b.name));
                if (out.address != 0) out.receiver = java.new_object(java.class_handle(b.class_name));
                else note("input: " + b.class_name + "." + b.name + b.signature + " is neither registered nor exported");
                return bound[&b] = out;
            };
            const auto deliver_input = [&]() -> bool {
                if (!adapter.deliver(host)) return false;  // what the app's Java side delivers (replies, state)
                if (!config_.input) return true;
                auto events = config_.input->drain();
                // A receiver keeps reporting once a second after it loses the sky (tunnels, garages): when fixes stop
                // arriving, the guest is told so instead of seeing the last fix frozen.
                const auto now_ms = static_cast<std::uint64_t>(
                    std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now().time_since_epoch()).count());
                bool fresh_fix = false;
                for (const auto& event : events)
                    if (const auto* fix = std::get_if<LocationFix>(&event)) { location_watch.observe(*fix, now_ms); fresh_fix = true; }
                if (!fresh_fix && !config_.input_bindings.location.empty())
                    if (const auto lost = location_watch.lost(now_ms)) events.emplace_back(*lost);
                for (const auto& event : events) {
                    const bool touch = std::holds_alternative<TouchEvent>(event);
                    if (const auto* fix = std::get_if<LocationFix>(&event)) adapter.location_delivered(*fix);
                    const auto bindings = config_.input_bindings.matching(event);
                    if (bindings.empty()) note(std::string("input: no binding for ") + (touch ? "touch" : "location") + " events");
                    for (const auto* binding : bindings) {
                    const auto target = resolve(*binding);
                    if (target.address == 0) continue;
                    const auto calls = native_call_count(*binding, event);
                    for (std::size_t part = 0; part < calls; ++part) {
                    std::string why;
                    const auto words = native_call_words(*binding, event, config_.input_bindings.actions, java.env(), target.receiver, why,
                                                         [&](std::size_t index, const std::string& text) -> std::uint32_t {
                        auto& handle = text_arguments[{binding, index}];
                        if (handle == 0) handle = java.new_string(text);
                        else if (!java.set_string_text(handle, text)) return 0;
                        return handle;
                    }, part);
                    if (!words) { note("input: " + why); break; }
                    note(std::string("input: first ") + (touch ? "touch" : "location") + " event -> " + binding->class_name + "." + binding->name);
                    const auto r = dispatcher.call(runtime, target.address, *words, stack_base + stack_bytes,
                                                   return_sentinel, config_.surface_call_budget);
                    if (r.stop != Stop::returned) {
                        log_(describe_state(runtime, dispatcher));
                        fail(stopped("input " + binding->name, r));
                        return false;
                    }
                    }
                    }
                }
                return true;
            };
            write_profile("init");  // constructors, JNI_OnLoad, nativeInit
            bool surfaces_ok = native_init_address != 0;
            if (surfaces_ok) {
                const auto surface = java.new_object(java.class_handle("android/view/Surface"));
                for (const auto& call : startup.surface_calls) {
                    if (!native(call.symbol, call.surface_argument ? std::vector<std::uint32_t>{surface} : std::vector<std::uint32_t>{})) {
                        surfaces_ok = false;
                        break;
                    }
                }
                for (std::uint32_t i = 0; surfaces_ok && i < config_.render_frames && !stop_.load(); ++i) {
                    if (!deliver_input()) return;
                    surfaces_ok = native(startup.render, {});
                    // GLSurfaceView swaps after onDrawFrame; the guest's frame call never calls eglSwapBuffers itself.
                    if (surfaces_ok) {
                        GlCall swap;
                        swap.op = GlOp::swap_buffers;
                        (config_.gl_backend ? *config_.gl_backend : static_cast<GlBackend&>(null_gl)).execute(swap);
                    }
                }
                if (!surfaces_ok) return;  // fail() already recorded the stop
            }
            // Run guest threads the library started, then deliver finished audio buffers.
            write_profile("render");  // the first `render_frames` frames and their input delivery
            const auto pumped = dispatcher.pump(runtime, config_.initializer_budget);
            log_("pump stop=" + std::to_string(static_cast<int>(pumped.stop)) + " retired=" + std::to_string(pumped.retired) +
                 " live_threads=" + std::to_string(threads.live_threads()) +
                 (pumped.message.empty() ? "" : " message=" + pumped.message));
            if (pumped.stop == Stop::unresolved_import) { fail("pump stopped at missing service " + pumped.import); return; }
            // Guest threads still busy when the budget ends are not a failure: the interactive loop keeps pumping them.
            if (pumped.stop != Stop::returned && pumped.stop != Stop::budget_exhausted) { fail(stopped("pump", pumped)); return; }
            const auto delivered = audio->pump(runtime, stack_base + stack_bytes, return_sentinel, config_.initializer_budget);
            if (delivered.error) { fail("audio callback: " + *delivered.error); return; }
            log_("audio callbacks=" + std::to_string(delivered.callbacks) + " players=" + std::to_string(audio->live_players()));
            // Interactive phase: input, one frame, guest threads and audio, repeated until stopped. Per-frame results
            // are not logged; a guest stop is reported like in the steps above.
            if (config_.interactive && surfaces_ok) {
                const auto* render = startup.render.empty() ? nullptr : image.find(startup.render);
                if (!render || render->type != 2) { state_.store(SessionState::finished); return; }
                const std::array<std::uint32_t, 2> render_args{java.env(), java.new_object(java.class_handle(startup.receiver_class))};
                log_("interactive");
                // Every `report_seconds` one line says where the time of the frames since the last line went.
                constexpr double report_seconds = 10.0;
                const auto seconds_since = [](std::chrono::steady_clock::time_point from) {
                    return std::chrono::duration<double>(std::chrono::steady_clock::now() - from).count();
                };
                auto report_start = std::chrono::steady_clock::now();
                std::uint64_t frames = 0, render_instructions = 0, pump_instructions = 0;
                double render_seconds = 0, render_worst = 0, pump_seconds = 0, audio_seconds = 0, idle_seconds = 0;
                dispatcher.reset_import_counts();
                log_(on_demand ? "frames on request" : "frames every interval");
                auto last_draw = std::chrono::steady_clock::now();
                auto next_draw = last_draw;  // a frame starts no earlier: `frame_interval_ms` after the previous one
                std::uint64_t drawn = 0;
                // Between frames the loop wakes for the guest's own timers (a thread in usleep or a timed wait) and
                // for input instead of sleeping out the interval, so the guest's threads keep their pace and a
                // request for a frame is served as soon as the interval allows.
                constexpr auto longest_nap = std::chrono::milliseconds(8);   // bounds the delay of host input
                constexpr auto shortest_nap = std::chrono::milliseconds(2);  // a guest polling faster does not spin the host
                const auto* program_exit = startup.exit_poll.empty() ? nullptr : image.find(startup.exit_poll);
                auto next_exit_check = std::chrono::steady_clock::now() + std::chrono::seconds(1);
                bool guest_exit = false;
                while (!stop_.load()) {
                    const auto frame_start = std::chrono::steady_clock::now();
                    if (!deliver_input()) return;
                    if (program_exit && program_exit->type == 2 && frame_start >= next_exit_check) {
                        next_exit_check = frame_start + std::chrono::seconds(1);
                        const auto asked = dispatcher.call(runtime, program_exit->address, render_args, stack_base + stack_bytes,
                                                           return_sentinel, config_.surface_call_budget);
                        if (asked.stop != Stop::returned) {
                            log_(describe_state(runtime, dispatcher));
                            fail(stopped(method_label(startup.exit_poll, "exit poll"), asked));
                            return;
                        }
                        if ((runtime.cpu.registers[0] & 0xff) == 1) { guest_exit = true; break; }
                    }
                    CallOutcome frame;
                    bool draw = frame_start >= next_draw && (!on_demand || adapter.frame_requested());
                    if (!draw && frame_start >= next_draw &&
                        seconds_since(last_draw) * 1000.0 >= static_cast<double>(config_.idle_redraw_ms)) {
                        // No request for a while: ask the guest the same question its callback would have. A frame
                        // drawn while it is not ready is only the clear colour, so that case draws nothing.
                        last_draw = frame_start;
                        const auto ready = adapter.frame_ready(host);
                        if (!ready) return;
                        draw = *ready;
                    }
                    if (draw) {
                        adapter.frame_drawn();
                        last_draw = frame_start;
                        next_draw = frame_start + std::chrono::milliseconds(config_.frame_interval_ms);
                        ++drawn;
                        frame = dispatcher.call(runtime, render->address, render_args, stack_base + stack_bytes,
                                                return_sentinel, config_.surface_call_budget);
                        if (frame.stop != Stop::returned) {
                            log_(describe_state(runtime, dispatcher));
                            fail(stopped(method_label(startup.render, "render"), frame));
                            return;
                        }
                        GlCall swap;
                        swap.op = GlOp::swap_buffers;
                        (config_.gl_backend ? *config_.gl_backend : static_cast<GlBackend&>(null_gl)).execute(swap);
                    }
                    const double rendered = seconds_since(frame_start);
                    const auto pump_start = std::chrono::steady_clock::now();
                    const auto background = dispatcher.pump(runtime, config_.initializer_budget);
                    if (background.stop == Stop::unresolved_import) { fail("pump stopped at missing service " + background.import); return; }
                    if (background.stop != Stop::returned && background.stop != Stop::budget_exhausted) { fail(stopped("pump", background)); return; }
                    // When the earliest guest timer is due; none pending: no bound from the guest.
                    std::optional<std::chrono::steady_clock::time_point> guest_timer;
                    if (const auto wake = threads.next_wake_ns(); wake != 0) {
                        const auto now = threads.now_ns();
                        guest_timer = std::chrono::steady_clock::now() + std::chrono::nanoseconds(wake > now ? wake - now : 0);
                    }
                    const double pumped_seconds = seconds_since(pump_start);
                    const auto audio_start = std::chrono::steady_clock::now();
                    const auto sound = audio->pump(runtime, stack_base + stack_bytes, return_sentinel, config_.initializer_budget);
                    if (sound.error) { fail("audio callback: " + *sound.error); return; }
                    audio_seconds += seconds_since(audio_start);
                    const auto idle_start = std::chrono::steady_clock::now();
                    if (background.stop != Stop::budget_exhausted) {
                        auto wake = idle_start + longest_nap;
                        if (guest_timer) wake = std::min(wake, std::max(idle_start + shortest_nap, *guest_timer));
                        if (!on_demand || adapter.frame_requested()) wake = std::min(wake, next_draw);
                        std::this_thread::sleep_until(wake);
                    }
                    idle_seconds += seconds_since(idle_start);
                    ++frames;
                    render_instructions += frame.retired;
                    pump_instructions += background.retired;
                    render_seconds += rendered;
                    if (draw) render_worst = std::max(render_worst, rendered);
                    pump_seconds += pumped_seconds;
                    if (const double elapsed = seconds_since(report_start); elapsed >= report_seconds) {
                        const double frames_drawn = static_cast<double>(std::max<std::uint64_t>(drawn, 1));
                        const auto share = [&](double total) { return std::to_string(total * 100.0 / elapsed); };
                        std::string line = "perf frames: " + std::to_string(static_cast<double>(drawn) / elapsed) + " fps (" +
                            std::to_string(drawn) + " drawn, " + std::to_string(adapter.take_frame_request_count()) + " requests, " +
                            std::to_string(frames) + " wakeups); per drawn frame render " +
                            std::to_string(render_seconds * 1000.0 / frames_drawn) + " ms (worst " +
                            std::to_string(render_worst * 1000.0) + "), " +
                            std::to_string(render_instructions / std::max<std::uint64_t>(drawn, 1)) + " instructions at " +
                            std::to_string(render_seconds > 0 ? static_cast<double>(render_instructions) / render_seconds / 1e6 : 0.0) +
                            " MIPS; time render " + share(render_seconds) + "%, threads " + share(pump_seconds) + "% (" +
                            std::to_string(pump_seconds > 0 ? static_cast<double>(pump_instructions) / pump_seconds / 1e6 : 0.0) +
                            " MIPS), audio " + share(audio_seconds) + "%, idle " + share(idle_seconds) + "%";
                        if (config_.perf_log) line += perf_details();
                        log_(line);
                        write_profile("frames");
                        dispatcher.reset_import_counts();
                        report_start = std::chrono::steady_clock::now();
                        frames = render_instructions = pump_instructions = drawn = 0;
                        render_seconds = render_worst = pump_seconds = audio_seconds = idle_seconds = 0;
                    }
                }
                log_(guest_exit ? "guest exit" : "stopped");
                if (guest_exit && config_.on_guest_exit) config_.on_guest_exit();
            }
        }
        state_.store(SessionState::finished);
    } catch (const std::exception& error) {
        fail(std::string("load failed: ") + error.what());
    }
}
}
