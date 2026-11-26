// SPDX-License-Identifier: GPL-2.0-or-later
// Native side of atlan.host.GuestActivity and atlan.host.HostHttp: one guest session per process, drawn with the
// device's GLES2 (platform/gles), heard through its OpenSL ES, fed with the activity's touches, location and
// network state. What the app itself is comes from make_app_adapter (android_app.hpp).
#include "android_app.hpp"
#include "egl_host.hpp"
#include "gles_backend.hpp"
#include "opensles_audio_sink.hpp"
#include "shell/input.hpp"
#include "shell/log_file.hpp"
#include "shell/session.hpp"
#include "shell/surface.hpp"
#include <android/log.h>
#include <android/native_window_jni.h>
#include <jni.h>
#include <pthread.h>
#include <atomic>
#include <map>
#include <memory>
#include <mutex>
#include <string>

namespace atlan::android {
namespace {
constexpr const char* log_tag = "atlan";
JavaVM* vm = nullptr;
jclass activity_class = nullptr, http_class = nullptr;
jmethodID on_guest_exit = nullptr, http_fetch = nullptr;
pthread_key_t detach_key;

// JNIEnv of the calling thread, attaching host threads (the guest thread, HTTP threads) on first use; they detach
// when they end.
JNIEnv* thread_env() {
    JNIEnv* env = nullptr;
    if (vm->GetEnv(reinterpret_cast<void**>(&env), JNI_VERSION_1_6) == JNI_OK) return env;
    if (vm->AttachCurrentThread(&env, nullptr) != JNI_OK) return nullptr;
    pthread_setspecific(detach_key, env);
    return env;
}

std::string text(JNIEnv* env, jstring s) {
    if (!s) return {};
    const char* chars = env->GetStringUTFChars(s, nullptr);
    std::string out = chars ? chars : "";
    if (chars) env->ReleaseStringUTFChars(s, chars);
    return out;
}

struct Host {
    std::mutex lock;  // session start/stop
    std::unique_ptr<shell::GuestSession> session;
    EglHost egl;
    std::unique_ptr<GlesBackend> gl;
    OpenSlesAudioSink audio;
    shell::InputQueue input;
    shell::SurfaceState view;                     // the view's size in pixels
    std::atomic<std::uint64_t> guest_surface{0};  // (width << 32) | height from the guest; 0 until it says
    std::atomic<bool> network{true};
    shell::SessionLogFile log_file;
    jobject activity{};  // global reference while a session runs
    // HTTP requests in flight in Java, by token
    std::mutex http_lock;
    std::map<jlong, std::function<void(shell::HttpResponse)>> http_pending;
    jlong http_next{1};

    void log(const std::string& line) {
        __android_log_write(ANDROID_LOG_INFO, log_tag, line.c_str());
        log_file.write(line);
    }
    shell::PixelSize touch_surface() const {
        const auto guest = guest_surface.load();
        if (guest != 0) return {static_cast<std::uint32_t>(guest >> 32), static_cast<std::uint32_t>(guest)};
        return view.get();
    }
};
Host& host() {
    static Host* h = new Host;  // lives as long as the process; the session thread may outlive static destruction
    return *h;
}

void java_fetch(shell::HttpRequest request, std::function<void(shell::HttpResponse)> done) {
    auto& h = host();
    JNIEnv* env = thread_env();
    if (!env) {
        shell::HttpResponse failed;
        failed.error = "no Java thread";
        done(std::move(failed));
        return;
    }
    jlong token;
    {
        std::lock_guard lock(h.http_lock);
        token = h.http_next++;
        h.http_pending[token] = std::move(done);
    }
    jstring method = env->NewStringUTF(request.method.c_str());
    jstring url = env->NewStringUTF(request.url.c_str());
    jclass string_class = env->FindClass("java/lang/String");
    jobjectArray headers = env->NewObjectArray(static_cast<jsize>(request.headers.size() * 2), string_class, nullptr);
    for (std::size_t i = 0; i < request.headers.size(); ++i) {
        jstring name = env->NewStringUTF(request.headers[i].first.c_str());
        jstring value = env->NewStringUTF(request.headers[i].second.c_str());
        env->SetObjectArrayElement(headers, static_cast<jsize>(2 * i), name);
        env->SetObjectArrayElement(headers, static_cast<jsize>(2 * i + 1), value);
        env->DeleteLocalRef(name);
        env->DeleteLocalRef(value);
    }
    jbyteArray body = nullptr;
    if (!request.body.empty()) {
        body = env->NewByteArray(static_cast<jsize>(request.body.size()));
        env->SetByteArrayRegion(body, 0, static_cast<jsize>(request.body.size()), reinterpret_cast<const jbyte*>(request.body.data()));
    }
    env->CallStaticVoidMethod(http_class, http_fetch, token, method, url, headers, body, static_cast<jint>(request.timeout_ms));
    const bool thrown = env->ExceptionCheck();
    if (thrown) env->ExceptionClear();
    for (jobject o : {static_cast<jobject>(method), static_cast<jobject>(url), static_cast<jobject>(string_class),
                      static_cast<jobject>(headers), static_cast<jobject>(body)})
        if (o) env->DeleteLocalRef(o);
    if (thrown) {
        std::function<void(shell::HttpResponse)> callback;
        {
            std::lock_guard lock(h.http_lock);
            if (auto it = h.http_pending.find(token); it != h.http_pending.end()) {
                callback = std::move(it->second);
                h.http_pending.erase(it);
            }
        }
        if (callback) {
            shell::HttpResponse failed;
            failed.error = "HostHttp.fetch threw";
            callback(std::move(failed));
        }
    }
}

shell::HttpFetch android_http() {
    auto plain = shell::plain_http_fetch();
    return [plain](shell::HttpRequest request, std::function<void(shell::HttpResponse)> done) {
        if (request.url.rfind("https://", 0) == 0) java_fetch(std::move(request), std::move(done));
        else plain(std::move(request), std::move(done));
    };
}

void native_done(JNIEnv* env, jclass, jlong token, jint outcome, jint status, jbyteArray body, jstring error) {
    auto& h = host();
    std::function<void(shell::HttpResponse)> callback;
    {
        std::lock_guard lock(h.http_lock);
        auto it = h.http_pending.find(token);
        if (it == h.http_pending.end()) return;
        callback = std::move(it->second);
        h.http_pending.erase(it);
    }
    shell::HttpResponse response;
    response.outcome = outcome == 0 ? shell::HttpResponse::Outcome::ok
                     : outcome == 2 ? shell::HttpResponse::Outcome::malformed_url
                                    : shell::HttpResponse::Outcome::transport_error;
    response.status = status;
    if (body) {
        response.body.resize(static_cast<std::size_t>(env->GetArrayLength(body)));
        env->GetByteArrayRegion(body, 0, static_cast<jsize>(response.body.size()), reinterpret_cast<jbyte*>(response.body.data()));
    }
    response.error = text(env, error);
    callback(std::move(response));
}

void native_start(JNIEnv* env, jclass, jobject activity, jstring data_root, jstring log_dir, jstring bindings,
                  jstring signatures, jint width, jint height) {
    auto& h = host();
    std::lock_guard lock(h.lock);
    if (h.session) return;  // one session per process
    const std::string root = text(env, data_root);
    if (const auto logs = text(env, log_dir); !logs.empty()) h.log_file.open(logs);
    h.activity = env->NewGlobalRef(activity);
    h.view.set({static_cast<std::uint32_t>(width), static_cast<std::uint32_t>(height)});
    h.egl.set_log([](const std::string& line) { host().log("gl " + line); });
    h.gl = std::make_unique<GlesBackend>(h.egl);
    h.gl->set_log([](const std::string& line) { host().log("gl " + line); });
    h.gl->set_surface_size(static_cast<std::uint32_t>(width), static_cast<std::uint32_t>(height));

    shell::SessionConfig config;
    config.data_root = root;
    config.audio_sink = &h.audio;
    config.gl_backend = h.gl.get();
    config.surface = &h.view;
    config.input = &h.input;
    config.interactive = true;
    config.render_on_demand = true;
    config.frame_interval_ms = 33;
    config.native_signatures = text(env, signatures);
    {
        std::string why;
        if (auto parsed = shell::parse_input_bindings(text(env, bindings), why)) {
            config.input_bindings = std::move(*parsed);
            h.log(config.input_bindings.empty() ? "input bindings missing" : "input bindings ok");
        } else {
            h.log("input bindings rejected: " + why);
        }
    }
    config.on_guest_exit = [] {
        auto& hh = host();
        JNIEnv* e = thread_env();
        if (e && hh.activity) {
            e->CallVoidMethod(hh.activity, on_guest_exit);
            if (e->ExceptionCheck()) e->ExceptionClear();
        }
    };
    HostServices services;
    services.http = android_http();
    services.network_available = [] { return host().network.load(); };
    services.set_guest_surface = [](std::int32_t w, std::int32_t hgt) {
        if (w <= 0 || hgt <= 0) return;
        auto& hh = host();
        hh.guest_surface.store((std::uint64_t{static_cast<std::uint32_t>(w)} << 32) | static_cast<std::uint32_t>(hgt));
        hh.gl->set_surface_size(static_cast<std::uint32_t>(w), static_cast<std::uint32_t>(hgt));
    };
    config.adapter = make_app_adapter(services, root);
    h.log("data root " + root + ", view " + std::to_string(width) + "x" + std::to_string(height));
    h.session = std::make_unique<shell::GuestSession>(config, [](const std::string& line) { host().log(line); });
    if (!h.session->start()) h.log("thread start failed");
}

void native_stop(JNIEnv* env, jclass) {
    auto& h = host();
    std::lock_guard lock(h.lock);
    if (h.session) {
        h.session->stop();
        h.session->join();
    }
    if (h.activity) {
        env->DeleteGlobalRef(h.activity);
        h.activity = nullptr;
    }
}

void native_set_window(JNIEnv* env, jclass, jobject surface) {
    ANativeWindow* window = surface ? ANativeWindow_fromSurface(env, surface) : nullptr;
    host().egl.set_window(window);
    if (window) ANativeWindow_release(window);  // set_window holds its own reference
}

void native_view_size(JNIEnv*, jclass, jint width, jint height) {
    if (width > 0 && height > 0) host().view.set({static_cast<std::uint32_t>(width), static_cast<std::uint32_t>(height)});
}

void native_touch(JNIEnv*, jclass, jint phase, jint pointer, jfloat x, jfloat y, jlong time_ms) {
    auto& h = host();
    const auto view = h.view.get();
    const shell::ViewRect shown{0, 0, static_cast<double>(view.width), static_cast<double>(view.height)};
    const auto point = shell::to_surface(x, y, shown, h.touch_surface());
    h.input.push(shell::TouchEvent{static_cast<shell::TouchPhase>(phase & 3), static_cast<std::uint32_t>(pointer),
                                   point.x, point.y, static_cast<std::uint64_t>(time_ms)});
}

void native_location(JNIEnv*, jclass, jdouble latitude, jdouble longitude, jdouble altitude, jdouble speed,
                     jdouble course, jdouble accuracy, jlong time_ms) {
    host().input.push(shell::LocationFix{latitude, longitude, altitude, speed, course, accuracy, static_cast<std::uint64_t>(time_ms)});
}

void native_network(JNIEnv*, jclass, jboolean available) { host().network.store(available == JNI_TRUE); }
}
}

extern "C" JNIEXPORT jint JNI_OnLoad(JavaVM* java_vm, void*) {
    using namespace atlan::android;
    vm = java_vm;
    pthread_key_create(&detach_key, [](void*) { vm->DetachCurrentThread(); });
    JNIEnv* env = nullptr;
    if (vm->GetEnv(reinterpret_cast<void**>(&env), JNI_VERSION_1_6) != JNI_OK) return JNI_ERR;
    jclass activity = env->FindClass("atlan/host/GuestActivity");
    jclass http = env->FindClass("atlan/host/HostHttp");
    if (!activity || !http) return JNI_ERR;
    activity_class = static_cast<jclass>(env->NewGlobalRef(activity));
    http_class = static_cast<jclass>(env->NewGlobalRef(http));
    on_guest_exit = env->GetMethodID(activity_class, "onGuestExit", "()V");
    http_fetch = env->GetStaticMethodID(http_class, "fetch", "(JLjava/lang/String;Ljava/lang/String;[Ljava/lang/String;[BI)V");
    if (!on_guest_exit || !http_fetch) return JNI_ERR;
    const JNINativeMethod activity_methods[] = {
        {"nativeStart", "(Latlan/host/GuestActivity;Ljava/lang/String;Ljava/lang/String;Ljava/lang/String;Ljava/lang/String;II)V",
         reinterpret_cast<void*>(native_start)},
        {"nativeStop", "()V", reinterpret_cast<void*>(native_stop)},
        {"nativeSetWindow", "(Landroid/view/Surface;)V", reinterpret_cast<void*>(native_set_window)},
        {"nativeViewSize", "(II)V", reinterpret_cast<void*>(native_view_size)},
        {"nativeTouch", "(IIFFJ)V", reinterpret_cast<void*>(native_touch)},
        {"nativeLocation", "(DDDDDDJ)V", reinterpret_cast<void*>(native_location)},
        {"nativeNetwork", "(Z)V", reinterpret_cast<void*>(native_network)},
    };
    const JNINativeMethod http_methods[] = {
        {"nativeDone", "(JII[BLjava/lang/String;)V", reinterpret_cast<void*>(native_done)},
    };
    if (env->RegisterNatives(activity_class, activity_methods, std::size(activity_methods)) != JNI_OK) return JNI_ERR;
    if (env->RegisterNatives(http_class, http_methods, std::size(http_methods)) != JNI_OK) return JNI_ERR;
    return JNI_VERSION_1_6;
}
