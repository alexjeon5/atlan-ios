// SPDX-License-Identifier: GPL-2.0-or-later
#include "adapters/atlan/atlan_adapter.hpp"
#include "adapters/atlan/charset_conversion.hpp"
#include "adapters/atlan/glyph_cache.hpp"
#include "adapters/atlan/quad_geometry.hpp"
#include "adapters/atlan/app_package.hpp"
#include "adapters/atlan/guest_ini.hpp"
#include "shell/input.hpp"
#include "adapters/atlan/java_services.hpp"
#include "shell/surface.hpp"
#include "atlan/jni.hpp"
#include "atlan/stdio.hpp"
#include "atlan/wav.hpp"
#include <filesystem>
#include <fstream>
#include <iterator>
#include <set>
#include <string_view>

namespace atlan::shell {
namespace {
constexpr char activity_class[] = "com/atlan/MainActivity";
constexpr char gl_thread_class[] = "com/atlan/AtlanGLThread";
constexpr char settings_file[] = "ATLAN/APP/system.ini";
constexpr char splash_sound_file[] = "ATLAN/UI/SPLASH/splash.wav";
}

AtlanAdapter::AtlanAdapter() = default;
AtlanAdapter::AtlanAdapter(Config config) : config_(std::move(config)) {}
AtlanAdapter::~AtlanAdapter() = default;

std::string AtlanAdapter::find_library(const std::string& data_root) const {
    return config_.map_root.empty() ? find_package(data_root) : find_package_in(config_.map_root);
}

std::string AtlanAdapter::missing_library() const { return "no guest library or package (ATLAN/APP/atlan.dat, *.apk)"; }

void AtlanAdapter::unpack_library(std::vector<std::byte>& bytes, const SessionLog& log) {
    if (!is_package(bytes)) return;
    auto package = open_package(bytes);
    log("package " + package.entry + (package.version.empty() ? "" : " version " + package.version) +
        " java_methods=" + std::to_string(package.java_methods.size()));
    bytes = std::move(package.library);
    java_methods_ = std::move(package.java_methods);
}

void AtlanAdapter::configure_files(GuestStdio& stdio, const SessionLog& log) {
    cached_lookup::attach(map_mode_, &stdio);
    if (!config_.map_root.empty()) {
        const std::filesystem::path state = config_.state_root.empty() ? stdio.host_path("ATLAN/NAND")
                                                                      : std::filesystem::path(config_.state_root);
        std::error_code error;
        std::filesystem::create_directories(state, error);
        stdio.add_mount("ATLAN/NAND", state);
        stdio.add_mount("ATLAN", config_.map_root);
        log("map folder " + config_.map_root);
    }
    // The guest's settings file names its folders by absolute Android paths, which differ per device and may be
    // edited by the user. Whatever ROOTPATH says is the ATLAN folder beneath the data root; any other absolute path
    // through a folder named "atlan" (any case) lands in that same folder.
    std::string settings;
    if (std::ifstream input(stdio.host_path(settings_file), std::ios::binary); input)
        settings.assign(std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>());
    if (const auto root = ini_value(settings, "ROOTPATH"); root && root->size() > 1 && root->front() == '/') {
        stdio.add_alias(*root, "ATLAN");
        log("data root " + *root);
    }
    stdio.add_alias("/storage/sdcard1/Atlan", "ATLAN");  // ROOTPATH of the stock settings: the map data folder
    stdio.add_alias("/data/app_storage/Atlan", "ATLAN/NAND");  // stock NANDPATH/CONFIGPATH/SDIPATH: app-written state
    stdio.add_alias("/finetmp/atlan", "ATLAN/NAND/finetmp");  // scratch folder
    stdio.add_anchor("atlan", "ATLAN");
    // The guest reads a copy of the settings file with two entries controlled here.
    // CLOUDMAPPATH: where downloaded map tiles would be kept. The stock value names a folder that does not exist,
    // which keeps the downloader off; a missing or writable value turns it on, and its map read thread is never
    // started here, so the first tile request would stop the guest. Always serve the stock value.
    // STARTMODEUSE: 1 leaves the choice of map (dynamic 3D, realtime 3D, classic 2D) to the head unit's launcher,
    // which does not exist here, so the map could never be changed. 0 lets the app offer it itself: a choice screen
    // at start (continues with the last map after a few seconds) and a map change button in the exit dialog.
    // Builds without the feature ignore the entry.
    stdio.add_read_filter(settings_file, [](std::string text) {
        return ini_set(ini_set(std::move(text), "CLOUDMAPPATH", "/map/CLOUD_MAP", '2'), "STARTMODEUSE", "0", '1');
    });
    use_connection_manager_ = ini_number(settings, "USECONNECTIONMANAGER").value_or(1) != 0;
    splash_sound_ = stdio.host_path(splash_sound_file);
}

void AtlanAdapter::install_java(AppHost& host, bool frame_requests) {
    JavaBootstrap& java = host.java();
    const SessionLog log = host.log();
    // The activity opens its splash screen before the native side starts, and the screen plays the start-up sound.
    if (std::error_code error; !splash_sound_.empty() && std::filesystem::is_regular_file(splash_sound_, error)) {
        std::string why;
        splash_stream_ = play_wav_file(host.audio(), splash_sound_, why);
        log(splash_stream_ ? "splash sound playing" : "splash sound: " + why);
    }
    // Host side of the Java callbacks the guest invokes. The surface size is handed to the embedder; the call
    // succeeds without other effect (a void method has no result to invent).
    java.register_method(activity_class, "setSurfaces", "(II)V",
                         [this, &host, log](JavaBootstrap::MethodCall& m) -> std::optional<std::string> {
        std::string args;
        for (auto a : m.args) args += (args.empty() ? "" : ",") + std::to_string(a);
        log("java MainActivity.setSurfaces(" + args + ")");
        if (const auto* surface = host.surface()) {
            const auto size = surface->get();
            log("host surface " + std::to_string(size.width) + "x" + std::to_string(size.height));
        }
        if (config_.on_set_surfaces && m.args.size() >= 2)
            config_.on_set_surfaces(static_cast<std::int32_t>(m.args[0]), static_cast<std::int32_t>(m.args[1]));
        m.result = 0;
        return std::nullopt;
    });
    // AtlanGLThread.callBackGLThread(): the guest's GL thread asks for a frame. The Java side answers
    //   if (isStateReady()) { if (!isGLInit()) setState(2); else { setState(3); requestRender(); } }
    // with the three natives below; the request is served by the session's interactive loop.
    is_state_ready_ = host.function("Java_com_atlan_AtlanGLThread_isStateReady");
    is_gl_init_ = host.function("Java_com_atlan_AtlanGLThread_isGLInit");
    set_state_ = host.function("Java_com_atlan_AtlanGLThread_setState");
    on_demand_ = frame_requests && is_state_ready_ != 0 && is_gl_init_ != 0 && set_state_ != 0;
    if (on_demand_) {
        java.register_method(gl_thread_class, "callBackGLThread", "()V",
                             [this, &host](JavaBootstrap::MethodCall& m) -> std::optional<std::string> {
            JavaBootstrap& java = host.java();
            const auto cls = java.class_handle(gl_thread_class);
            const auto call_native = [&](std::uint32_t address, std::optional<std::uint32_t> argument, std::uint32_t& result) -> std::optional<std::string> {
                std::vector<std::uint32_t> words{java.env(), cls};
                if (argument) words.push_back(*argument);
                std::string message;
                const auto r0 = host.call_nested(address, words, message);
                if (!r0) return "callBackGLThread: a GL thread native did not return" + (message.empty() ? std::string() : ": " + message);
                result = *r0;
                return std::nullopt;
            };
            std::uint32_t ready = 0, initialised = 0, ignored = 0;
            if (auto error = call_native(is_state_ready_, std::nullopt, ready)) return error;
            if ((ready & 0xff) != 0) {
                if (auto error = call_native(is_gl_init_, std::nullopt, initialised)) return error;
                if (auto error = call_native(set_state_, (initialised & 0xff) != 0 ? 3u : 2u, ignored)) return error;
                if ((initialised & 0xff) != 0) { render_requested_ = true; ++render_requests_; }
            }
            m.result = 0;
            return std::nullopt;
        });
    }
    // Network-related Java methods (IsConnected, SendMsgFromAtlan, AtlanNetService) and the replies they queue.
    JavaServices::Config services;
    services.http = config_.http;
    services.network_available = config_.network_available;
    services.use_connection_manager = use_connection_manager_;
    services.log = log;
    services.on_home = config_.on_home;
    services.on_splash_end = [this, log](std::uint32_t param) {
        if (param == 0 || !splash_stream_) return;  // 0 only closes the splash screen; its sound plays on
        splash_stream_.reset();
        log("splash sound stopped by the app");
    };
    java_services_ = std::make_unique<JavaServices>(services);
    java_services_->install(java, [&host](const std::string& cls, const std::string& name, const std::string& signature) {
        return host.native_method(cls, name, signature);
    });
    // Other void UI callbacks the guest raises: recorded in the log, no host effect yet.
    struct Callback { const char* cls; const char* name; const char* signature; };
    // JniIsArService: the guest reports whether its AR service is on; the Java side only stores the value for its
    // camera overlay, which the host does not have.
    for (const Callback& cb : {Callback{activity_class, "JniArSettingData", "(IIII)V"},
                               Callback{activity_class, "JniIsArService", "(I)V"},
                               Callback{activity_class, "SendMsgFromTPEGD", "(III[B)V"},
                               Callback{gl_thread_class, "callBackGLThread", "()V"}}) {
        if (on_demand_ && std::string_view(cb.name) == "callBackGLThread") continue;
        java.register_method(cb.cls, cb.name, cb.signature,
                             [log, logged = std::set<std::string>(), label = std::string(cb.cls).substr(std::string(cb.cls).rfind('/') + 1) + "." + cb.name](JavaBootstrap::MethodCall& m) mutable -> std::optional<std::string> {
            std::string args;
            for (auto a : m.args) args += (args.empty() ? "" : ",") + std::to_string(a);
            if (logged.insert(label + "(" + args + ")").second) log("java " + label + "(" + args + ")");  // periodic; once each
            m.result = 0;
            return std::nullopt;
        });
    }
}

void AtlanAdapter::uninstall_java() {
    java_services_.reset();
    splash_stream_.reset();
}

std::vector<NativeSet> AtlanAdapter::native_sets() const {
    std::vector<NativeSet> sets;
    sets.push_back(geometry::quad_geometry_set());
    for (auto& set : charset::conversion_sets(config_.exhaustive_native_checks)) sets.push_back(std::move(set));
    for (auto& set : speed::speed_readout_sets(fix_speed_)) sets.push_back(std::move(set));
    sets.push_back(glyph_cache::glyph_cache_set());
    for (auto& set : cached_lookup::cached_lookup_sets(map_mode_)) sets.push_back(std::move(set));
    return sets;
}

void AtlanAdapter::location_delivered(const LocationFix& fix) {
    fix_speed_->store(speed::fix_kmh(fix.speed_mps, fix.accuracy_m).value_or(-1), std::memory_order_relaxed);
}
AppStartup AtlanAdapter::startup() const {
    AppStartup startup;
    startup.receiver_class = activity_class;
    startup.init = "Java_com_atlan_MainActivity_nativeInit";
    startup.surface_calls = {{"Java_com_atlan_AtlanUISurfaceView_createUISF", true},
                             {"Java_com_atlan_AtlanGLSurfaceView_createGLSF", false},
                             {"Java_com_atlan_AtlanUISurfaceView_changedUISF", false}};
    startup.render = "Java_com_atlan_AtlanGLSurfaceView_renderGLSF";
    // The Java side polls this once a second and finishes the activity when the user chose to quit.
    startup.exit_poll = "Java_com_atlan_MainActivity_nativeIsProgramExit";
    return startup;
}

bool AtlanAdapter::deliver(AppHost& host) {
    cached_lookup::arm(map_mode_);  // start-up is over
    if (!java_services_) return true;
    // Replies of the Java side (online state, finished HTTP requests), on the guest thread like input.
    for (const auto& due : java_services_->take_due())
        if (!host.call("java " + due.what, due.address, due.words)) return false;
    return true;
}

std::uint64_t AtlanAdapter::take_frame_request_count() {
    const auto count = render_requests_;
    render_requests_ = 0;
    return count;
}

std::optional<bool> AtlanAdapter::frame_ready(AppHost& host) {
    if (!on_demand_) return true;
    // The same question callBackGLThread answers. A frame drawn while the guest is not ready shows only the clear
    // colour, so that case draws nothing.
    JavaBootstrap& java = host.java();
    const auto cls = java.class_handle(gl_thread_class);
    const auto ask = [&](std::uint32_t address, std::optional<std::uint32_t> argument) {
        std::vector<std::uint32_t> words{java.env(), cls};
        if (argument) words.push_back(*argument);
        return host.call("GL thread state", address, words);
    };
    const auto ready = ask(is_state_ready_, std::nullopt);
    if (!ready) return std::nullopt;
    if ((*ready & 0xff) == 0) return false;
    const auto initialised = ask(is_gl_init_, std::nullopt);
    if (!initialised || !ask(set_state_, (*initialised & 0xff) != 0 ? 3u : 2u)) return std::nullopt;
    return (*initialised & 0xff) != 0;
}
}
