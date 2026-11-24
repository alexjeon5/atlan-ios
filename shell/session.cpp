// SPDX-License-Identifier: GPL-2.0-or-later
#include "shell/session.hpp"
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
#include "atlan/opensles.hpp"
#include "atlan/string_services.hpp"
#include "atlan/lifetime.hpp"
#include "atlan/runtime.hpp"
#include "atlan/threads.hpp"
#include <array>
#include <exception>
#include <map>
#include <optional>
#include <random>
#include <fstream>
#include <pthread.h>
#include <stdexcept>
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
std::string describe_stop(const std::string& label, const CallOutcome& outcome) {
    std::string line = label + " stopped after " + std::to_string(outcome.retired) + " instructions";
    if (outcome.stop == Stop::unresolved_import) line += " missing_service=" + outcome.import;
    else if (outcome.stop == Stop::service_error) line += " failing_service=" + outcome.import + " " + outcome.message;
    else if (outcome.stop == Stop::budget_exhausted) line += " budget_exhausted";
    else line += std::string(" event=") + event_name(outcome.cpu.event);
    return line;
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
        auto image = elf::load(read_library(config_.library_path), options);
        log_("loaded mapped_bytes=" + std::to_string(image.mapped_bytes) +
             " exports=" + std::to_string(image.exports.size()) +
             " imports_trapped=" + std::to_string(image.trapped_imports.size()) +
             " initializers=" + std::to_string(image.initializers.size()));
        Runtime runtime;
        ImportDispatcher dispatcher;
        ObjectLifetime lifetime;
        lifetime.install(dispatcher);
        install_libc_services(dispatcher);
        install_time_services(dispatcher, guard_page + 0x400);
        install_libc_extra(dispatcher);
        GuestHeap heap;
        install_heap_services(dispatcher, heap);
        install_string_services(dispatcher);
        install_printf_services(dispatcher);
        const std::map<std::string, std::string> properties;
        install_system_properties(dispatcher, properties, [this](const std::string& name, bool known) {
            log_("property " + name + (known ? "" : " (unset)"));
        });
        std::optional<GuestStdio> stdio;
        if (!config_.data_root.empty()) {
            stdio.emplace(config_.data_root, guard_page + 0x200, [this](const std::string& path, const std::string& mode, bool ok) {
                log_("fopen " + path + " mode=" + mode + (ok ? " ok" : " failed"));
            });
            stdio->add_alias("/storage/sdcard1/Atlan", "ATLAN");  // ROOTPATH in system.ini: the map data folder
            stdio->add_alias("/data/app_storage/Atlan", "ATLAN/NAND");  // NANDPATH/CONFIGPATH/SDIPATH: app-written state
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
        SemaphoreTable semaphores(&threads);
        semaphores.install(dispatcher);
        ThreadKeys thread_keys(&threads);
        thread_keys.install(dispatcher);
        AndroidLog android_log([this](const LogEntry& entry) {
            log_("android_log " + std::to_string(entry.priority) + " " + entry.tag + ": " + entry.message);
        });
        android_log.install(dispatcher);
        dispatcher.attach(image);
        auto initializers = std::move(image.initializers);
        const auto* on_load = image.find("JNI_OnLoad");
        const std::uint32_t on_load_address = on_load && on_load->type == 2 ? on_load->address : 0;
        const auto* native_init = image.find("Java_com_atlan_MainActivity_nativeInit");
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
        std::optional<OpenSles> audio;
        {
            OpenSles::Config audio_config;
            audio_config.sink = config_.audio_sink ? config_.audio_sink : &fallback_sink;
            audio_config.trace = [this](const std::string& line) { log_("opensles " + line); };
            audio.emplace(runtime, dispatcher, audio_config);
        }
        for (std::size_t i = 0; i < initializers.size(); ++i) {
            if (stop_.load()) { log_("stopped before initializer " + std::to_string(i)); state_.store(SessionState::finished); return; }
            const auto outcome = dispatcher.call(runtime, initializers[i], {}, stack_base + stack_bytes,
                                                 return_sentinel, config_.initializer_budget);
            if (outcome.stop != Stop::returned) { fail(describe_stop("initializer[" + std::to_string(i) + "]", outcome)); return; }
        }
        log_("initializers_completed=" + std::to_string(initializers.size()));
        if (on_load_address != 0 && !stop_.load()) {
            JavaBootstrap java(runtime, dispatcher, JavaBootstrap::Config{});
            // Host side of the Java callbacks the guest invokes. The UI layer does not exist yet, so the call is
            // logged with its arguments and succeeds without effect (a void method has no result to invent).
            java.register_method("com/atlan/MainActivity", "setSurfaces", "(II)V",
                                 [this](JavaBootstrap::MethodCall& m) -> std::optional<std::string> {
                std::string args;
                for (auto a : m.args) args += (args.empty() ? "" : ",") + std::to_string(a);
                log_("java MainActivity.setSurfaces(" + args + ")");
                m.result = 0;
                return std::nullopt;
            });
            // Other void UI callbacks the guest raises: recorded in the log, no host effect yet.
            struct Callback { const char* cls; const char* name; const char* signature; };
            for (const Callback& cb : {Callback{"com/atlan/MainActivity", "SendMsgFromAtlan", "(III[B)V"},
                                       Callback{"com/atlan/MainActivity", "JniArSettingData", "(IIII)V"},
                                       Callback{"com/atlan/MainActivity", "SendMsgFromTPEGD", "(III[B)V"},
                                       Callback{"com/atlan/AtlanGLThread", "callBackGLThread", "()V"}}) {
                java.register_method(cb.cls, cb.name, cb.signature,
                                     [this, label = std::string(cb.cls).substr(std::string(cb.cls).rfind('/') + 1) + "." + cb.name](JavaBootstrap::MethodCall& m) -> std::optional<std::string> {
                    std::string args;
                    for (auto a : m.args) args += (args.empty() ? "" : ",") + std::to_string(a);
                    log_("java " + label + "(" + args + ")");
                    m.result = 0;
                    return std::nullopt;
                });
            }
            const std::array<std::uint32_t, 2> args{java.vm(), 0};
            const auto outcome = dispatcher.call(runtime, on_load_address, args, stack_base + stack_bytes,
                                                 return_sentinel, config_.initializer_budget);
            for (const auto& cls : java.classes()) log_("jni class " + cls);
            for (const auto& m : java.native_methods()) log_("jni native " + m.class_name + "." + m.name + m.signature);
            if (outcome.stop != Stop::returned) { fail(describe_stop("JNI_OnLoad", outcome)); return; }
            log_("JNI_OnLoad returned 0x" + hex(runtime.cpu.registers[0]));
            if (native_init_address != 0) {
                const std::array<std::uint32_t, 2> init_args{java.env(), java.new_object(java.class_handle("com/atlan/MainActivity"))};
                const auto started = dispatcher.call(runtime, native_init_address, init_args, stack_base + stack_bytes,
                                                     return_sentinel, config_.native_init_budget);
                if (started.stop != Stop::returned) { fail(describe_stop("nativeInit", started)); return; }
                log_("nativeInit returned 0x" + hex(runtime.cpu.registers[0]));
            } else {
                log_("nativeInit not exported");
            }
            // Run guest threads the library started, then deliver finished audio buffers.
            const auto pumped = dispatcher.pump(runtime, config_.initializer_budget);
            log_("pump stop=" + std::to_string(static_cast<int>(pumped.stop)) + " retired=" + std::to_string(pumped.retired) +
                 " live_threads=" + std::to_string(threads.live_threads()) +
                 (pumped.message.empty() ? "" : " message=" + pumped.message));
            if (pumped.stop == Stop::unresolved_import) { fail("pump stopped at missing service " + pumped.import); return; }
            if (pumped.stop != Stop::returned) { fail(describe_stop("pump", pumped)); return; }
            const auto delivered = audio->pump(runtime, stack_base + stack_bytes, return_sentinel, config_.initializer_budget);
            if (delivered.error) { fail("audio callback: " + *delivered.error); return; }
            log_("audio callbacks=" + std::to_string(delivered.callbacks) + " players=" + std::to_string(audio->live_players()));
        }
        state_.store(SessionState::finished);
    } catch (const std::exception& error) {
        fail(std::string("load failed: ") + error.what());
    }
}
}
