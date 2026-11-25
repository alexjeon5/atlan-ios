// SPDX-License-Identifier: GPL-2.0-or-later
#include "shell/session.hpp"
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
#include "atlan/opensles.hpp"
#include "atlan/string_services.hpp"
#include "atlan/lifetime.hpp"
#include "atlan/runtime.hpp"
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
std::string describe_stop(const std::string& label, const CallOutcome& outcome) {
    std::string line = label + " stopped after " + std::to_string(outcome.retired) + " instructions";
    if (outcome.stop == Stop::unresolved_import) line += " missing_service=" + outcome.import;
    else if (outcome.stop == Stop::service_error) line += " failing_service=" + outcome.import + " " + outcome.message;
    else if (outcome.stop == Stop::budget_exhausted) line += " budget_exhausted";
    else line += std::string(" event=") + event_name(outcome.cpu.event) + " address=0x" + hex(outcome.cpu.address) +
                 " access=" + std::to_string(static_cast<int>(outcome.cpu.access));
    return line;
}
std::string describe_state(const Runtime& runtime, const ImportDispatcher& dispatcher) {
    std::string out = "state pc=0x" + hex(runtime.pc()) + " lr=0x" + hex(runtime.cpu.registers[14]) +
                      " sp=0x" + hex(runtime.cpu.registers[13]);
    for (unsigned i = 0; i < 4; ++i) out += " r" + std::to_string(i) + "=0x" + hex(runtime.cpu.registers[i]);
    std::vector<std::pair<std::string, std::uint64_t>> top(dispatcher.import_counts().begin(), dispatcher.import_counts().end());
    std::sort(top.begin(), top.end(), [](const auto& a, const auto& b) { return a.second > b.second; });
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
        dispatcher.set_block_cache(config_.block_cache);
        ObjectLifetime lifetime;
        lifetime.install(dispatcher);
        install_libc_services(dispatcher);
        install_time_services(dispatcher, guard_page + 0x400);
        install_libc_extra(dispatcher);
        GuestHeap heap;
        install_heap_services(dispatcher, heap);
        install_string_services(dispatcher);
        install_printf_services(dispatcher);
        install_console_services(dispatcher, guard_page + 0x100, guard_page + 0x200, [this](const std::string& text) { log_("guest stdout " + text); });
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
            stdio->add_alias("/finetmp/atlan", "ATLAN/NAND/finetmp");  // scratch folder
            // CLOUDMAPPATH in system.ini: the guest open(O_CREAT)s mapversion.idx here without a mkdir and reads the
            // result unchecked, so a missing folder showed up as endless "read error: Bad file descriptor".
            stdio->add_alias("/map/CLOUD_MAP", "ATLAN/NAND/CLOUD_MAP", true);
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
        NullGlBackend null_gl;
        Gles2 gles(config_.gl_backend ? *config_.gl_backend : static_cast<GlBackend&>(null_gl), Gles2::Config{guard_page + 0x800});
        gles.install(dispatcher);
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
        // A thread that waits on its own mutex for a buffer-queue callback to unlock it (synchronous sound playback):
        // deliver due callbacks while it waits, and keep waiting while the sink still has buffers to play.
        mutexes.set_idle_wait([&audio, this](ImportCall& call) {
            const auto delivered = audio->pump_nested(call.runtime());
            if (delivered.error) { log_("audio callback: " + *delivered.error); return MutexTable::Idle::none; }
            if (delivered.callbacks != 0) return MutexTable::Idle::progressed;
            return delivered.pending != 0 ? MutexTable::Idle::waiting : MutexTable::Idle::none;
        });
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
                if (config_.surface) {
                    const auto size = config_.surface->get();
                    log_("host surface " + std::to_string(size.width) + "x" + std::to_string(size.height));
                }
                if (config_.on_set_surfaces && m.args.size() >= 2)
                    config_.on_set_surfaces(static_cast<std::int32_t>(m.args[0]), static_cast<std::int32_t>(m.args[1]));
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
            // Surface and render entry points (plain Java_ exports: (env, this[, surface]) -> int).
            const auto native = [&](const char* name, std::vector<std::uint32_t> extra) -> bool {
                const auto* sym = image.find(name);
                if (!sym || sym->type != 2) { log_(std::string(name) + " not exported"); return false; }
                std::vector<std::uint32_t> call_args{java.env(), java.new_object(java.class_handle("com/atlan/MainActivity"))};
                call_args.insert(call_args.end(), extra.begin(), extra.end());
                dispatcher.reset_import_counts();
                const auto perf_start = std::chrono::steady_clock::now();
                const auto r = dispatcher.call(runtime, sym->address, call_args, stack_base + stack_bytes,
                                               return_sentinel, config_.surface_call_budget);
                if (config_.perf_log) {
                    const double seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - perf_start).count();
                    std::string line = std::string("perf ") + name + ": " + std::to_string(r.retired) + " instructions, " +
                                       std::to_string(seconds) + " s, " + std::to_string(seconds > 0 ? r.retired / seconds / 1e6 : 0.0) + " MIPS";
                    if (const auto* runner = dispatcher.block_runner()) {
                        const auto& st = runner->stats();
                        line += "; blocks built " + std::to_string(st.blocks_built) + ", flushes " + std::to_string(st.flushes) +
                                ", without fast handler " + std::to_string(st.generic) + ", fallbacks " + std::to_string(st.fallbacks) +
                                ", single steps " + std::to_string(st.slow_steps) + "; top forms without fast handler:";
                        const auto forms = runner->generic_forms();
                        for (std::size_t i = 0; i < forms.size() && i < 24; ++i) line += " " + forms[i].first + "=" + std::to_string(forms[i].second);
                    }
                    log_(line);
                }
                if (r.stop != Stop::returned) {
                    log_(describe_state(runtime, dispatcher));  // where it stopped: pc, lr, sp, r0-r3, busiest imports
                    fail(describe_stop(name, r));
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
            const auto note = [&](const std::string& line) { if (input_notes.insert(line).second) log_(line); };
            const auto resolve = [&](const NativeBinding& b) -> BoundNative {
                if (const auto it = bound.find(&b); it != bound.end()) return it->second;
                BoundNative out;
                for (const auto& m : java.native_methods())
                    if (m.class_name == b.class_name && m.name == b.name && m.signature == b.signature) out.address = m.address;
                if (out.address == 0) {
                    std::string symbol = "Java_";
                    for (char c : b.class_name) { if (c == '/') symbol += '_'; else if (c == '_') symbol += "_1"; else symbol += c; }
                    symbol += '_';
                    for (char c : b.name) { if (c == '_') symbol += "_1"; else symbol += c; }
                    if (const auto* sym = image.find(symbol); sym && sym->type == 2) out.address = sym->address;
                }
                if (out.address != 0) out.receiver = java.new_object(java.class_handle(b.class_name));
                else note("input: " + b.class_name + "." + b.name + b.signature + " is neither registered nor exported");
                return bound[&b] = out;
            };
            const auto deliver_input = [&]() -> bool {
                if (!config_.input) return true;
                for (const auto& event : config_.input->drain()) {
                    const bool touch = std::holds_alternative<TouchEvent>(event);
                    const auto bindings = config_.input_bindings.matching(event);
                    if (bindings.empty()) note(std::string("input: no binding for ") + (touch ? "touch" : "location") + " events");
                    for (const auto* binding : bindings) {
                    const auto target = resolve(*binding);
                    if (target.address == 0) continue;
                    std::string why;
                    const auto words = native_call_words(*binding, event, config_.input_bindings.actions, java.env(), target.receiver, why,
                                                         [&](std::size_t index, const std::string& text) -> std::uint32_t {
                        auto& handle = text_arguments[{binding, index}];
                        if (handle == 0) handle = java.new_string(text);
                        else if (!java.set_string_text(handle, text)) return 0;
                        return handle;
                    });
                    if (!words) { note("input: " + why); continue; }
                    note(std::string("input: first ") + (touch ? "touch" : "location") + " event -> " + binding->class_name + "." + binding->name);
                    const auto r = dispatcher.call(runtime, target.address, *words, stack_base + stack_bytes,
                                                   return_sentinel, config_.surface_call_budget);
                    if (r.stop != Stop::returned) {
                        log_(describe_state(runtime, dispatcher));
                        fail(describe_stop("input " + binding->name, r));
                        return false;
                    }
                    }
                }
                return true;
            };
            bool surfaces_ok = native_init_address != 0;
            if (surfaces_ok) {
                const auto surface = java.new_object(java.class_handle("android/view/Surface"));
                surfaces_ok = native("Java_com_atlan_AtlanUISurfaceView_createUISF", {surface}) &&
                              native("Java_com_atlan_AtlanGLSurfaceView_createGLSF", {}) &&
                              native("Java_com_atlan_AtlanUISurfaceView_changedUISF", {});
                for (std::uint32_t i = 0; surfaces_ok && i < config_.render_frames && !stop_.load(); ++i) {
                    if (!deliver_input()) return;
                    surfaces_ok = native("Java_com_atlan_AtlanGLSurfaceView_renderGLSF", {});
                    // GLSurfaceView swaps after onDrawFrame; the guest's renderGLSF never calls eglSwapBuffers itself.
                    if (surfaces_ok) {
                        GlCall swap;
                        swap.op = GlOp::swap_buffers;
                        (config_.gl_backend ? *config_.gl_backend : static_cast<GlBackend&>(null_gl)).execute(swap);
                    }
                }
                if (!surfaces_ok) return;  // fail() already recorded the stop
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
            // Interactive phase: input, one frame, guest threads and audio, repeated until stopped. Per-frame results
            // are not logged; a guest stop is reported like in the steps above.
            if (config_.interactive && surfaces_ok) {
                const auto* render = image.find("Java_com_atlan_AtlanGLSurfaceView_renderGLSF");
                if (!render || render->type != 2) { state_.store(SessionState::finished); return; }
                const std::array<std::uint32_t, 2> render_args{java.env(), java.new_object(java.class_handle("com/atlan/MainActivity"))};
                log_("interactive");
                while (!stop_.load()) {
                    const auto frame_start = std::chrono::steady_clock::now();
                    if (!deliver_input()) return;
                    const auto frame = dispatcher.call(runtime, render->address, render_args, stack_base + stack_bytes,
                                                       return_sentinel, config_.surface_call_budget);
                    if (frame.stop != Stop::returned) {
                        log_(describe_state(runtime, dispatcher));
                        fail(describe_stop("renderGLSF", frame));
                        return;
                    }
                    GlCall swap;
                    swap.op = GlOp::swap_buffers;
                    (config_.gl_backend ? *config_.gl_backend : static_cast<GlBackend&>(null_gl)).execute(swap);
                    const auto background = dispatcher.pump(runtime, config_.initializer_budget);
                    if (background.stop == Stop::unresolved_import) { fail("pump stopped at missing service " + background.import); return; }
                    if (background.stop != Stop::returned && background.stop != Stop::budget_exhausted) { fail(describe_stop("pump", background)); return; }
                    const auto sound = audio->pump(runtime, stack_base + stack_bytes, return_sentinel, config_.initializer_budget);
                    if (sound.error) { fail("audio callback: " + *sound.error); return; }
                    std::this_thread::sleep_until(frame_start + std::chrono::milliseconds(config_.frame_interval_ms));
                }
                log_("stopped");
            }
        }
        state_.store(SessionState::finished);
    } catch (const std::exception& error) {
        fail(std::string("load failed: ") + error.what());
    }
}
}
