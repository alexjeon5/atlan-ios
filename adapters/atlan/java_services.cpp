// SPDX-License-Identifier: GPL-2.0-or-later
#include "adapters/atlan/java_services.hpp"
#include "atlan/jni.hpp"
#include <algorithm>
#include <chrono>
#include <deque>
#include <mutex>
#include <optional>
#include <set>

namespace atlan::shell {
namespace {
constexpr std::uint32_t msg_navi_start = 17;         // SendMsgFromAtlan: the guest's navigation has started
constexpr std::uint32_t msg_home = 5;                // SendMsgFromAtlan: the home button was pressed (param: its mode)
constexpr std::uint32_t msg_splash_end = 13;         // SendMsgFromAtlan: 0 hides the splash screen, else stops its sound
constexpr std::uint32_t msg_online_state = 0x429;    // nativeReceiveMsgData: REC_ONLINE_STATE, param 1 connected
constexpr std::uint64_t java_timeout_cap_ms = 13000;  // AtlanNetService caps every timeout at 13 s
constexpr char net_class[] = "com/atlan/AtlanNetService";
constexpr char activity_class[] = "com/atlan/MainActivity";
}

struct JavaServices::State {
    Config config;
    JavaBootstrap* java{};
    FindNative find_native;
    std::uint32_t activity{};  // receiver of MainActivity natives
    bool navi_started{};
    std::optional<bool> reported;  // online state last sent to the guest
    std::deque<NativeCall> due;     // guest thread only
    struct Reply {
        std::string what;
        std::vector<std::uint8_t> body;
        std::uint32_t status{}, id{};
    };
    std::mutex replies_mutex;
    std::deque<Reply> replies;  // filled by request threads
    std::set<std::string> logged;

    void log(const std::string& line) const { if (config.log) config.log(line); }
    void log_once(const std::string& line) { if (logged.insert(line).second) log(line); }
    bool available() const { return !config.network_available || config.network_available(); }
    void queue_online_state() {
        const bool online = available();
        reported = online;
        const auto address = find_native(activity_class, "nativeReceiveMsgData", "(III[B)V");
        if (address == 0) { log_once("java MainActivity.nativeReceiveMsgData is not registered"); return; }
        if (activity == 0) activity = java->new_object(java->class_handle(activity_class));
        log(std::string("java online state ") + (online ? "connected" : "disconnected"));
        due.push_back({"nativeReceiveMsgData(REC_ONLINE_STATE)", address,
                       {java->env(), activity, msg_online_state, online ? 1U : 0U, 0, 0}});
    }
    void request(HttpRequest request, std::uint32_t id) {
        const std::string what = request.method + " " + url_for_log(request.url);
        log("http " + what + " id=" + std::to_string(id) + " timeout=" + std::to_string(request.timeout_ms) + "ms" +
            (request.body.empty() ? "" : " body=" + std::to_string(request.body.size()) + "B"));
        const bool post = request.method == "POST";
        const auto started = std::chrono::steady_clock::now();
        auto http = config.http ? config.http : plain_http_fetch();
        // The reply outlives nothing but the shared state: a session that ended drops it.
        std::weak_ptr<State> self = self_;
        http(std::move(request), [self, what, id, post, started](HttpResponse response) {
            const auto state = self.lock();
            if (!state) return;
            const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - started).count();
            Reply reply{what, {}, 0, id};
            std::string result;
            switch (response.outcome) {
            case HttpResponse::Outcome::malformed_url: reply.status = 4; result = "malformed URL"; break;
            case HttpResponse::Outcome::transport_error: reply.status = 0; result = response.error; break;
            case HttpResponse::Outcome::ok:
                result = "HTTP " + std::to_string(response.status) + " " + std::to_string(response.body.size()) + "B";
                // GET: HttpURLConnection.getInputStream() throws for 4xx/5xx. POST: only 2xx counts as success.
                if (post ? response.status >= 200 && response.status < 300 : response.status < 400) {
                    reply.status = 1;
                    reply.body = std::move(response.body);
                }
                break;
            }
            state->log("http " + what + " id=" + std::to_string(id) + " -> " + result + " in " + std::to_string(ms) + "ms status=" +
                       std::to_string(reply.status));
            std::lock_guard lock(state->replies_mutex);
            state->replies.push_back(std::move(reply));
        });
    }
    std::weak_ptr<State> self_;
};

JavaServices::JavaServices(Config config) : state_(std::make_shared<State>()) {
    state_->config = std::move(config);
    state_->self_ = state_;
}
JavaServices::~JavaServices() = default;

void JavaServices::install(JavaBootstrap& java, FindNative find_native) {
    auto* state = state_.get();
    state->java = &java;
    state->find_native = std::move(find_native);
    const auto args_text = [](const JavaBootstrap::MethodCall& m) {
        std::string args;
        for (auto a : m.args) args += (args.empty() ? "" : ",") + std::to_string(a);
        return args;
    };
    // int MainActivity.IsConnected()
    (void)java.register_method(activity_class, "IsConnected", "()I", [state](JavaBootstrap::MethodCall& m) -> std::optional<std::string> {
        const bool online = state->available();
        state->log_once(std::string("java MainActivity.IsConnected() -> ") + (online ? "1" : "0"));
        m.result = online ? 1 : 0;
        return std::nullopt;
    });
    // void MainActivity.SendMsgFromAtlan(int id, int param, int size, byte[] data): broadcast to other apps on Android.
    (void)java.register_method(activity_class, "SendMsgFromAtlan", "(III[B)V", [state, args_text](JavaBootstrap::MethodCall& m) -> std::optional<std::string> {
        state->log_once("java MainActivity.SendMsgFromAtlan(" + args_text(m) + ")");  // periodic; once each
        if (!m.args.empty() && m.args[0] == msg_navi_start) {
            state->navi_started = true;
            if (state->config.use_connection_manager) state->queue_online_state();
        }
        if (!m.args.empty() && m.args[0] == msg_home) {
            state->log("java home button");
            if (state->config.on_home) state->config.on_home();
        }
        if (m.args.size() > 1 && m.args[0] == msg_splash_end && state->config.on_splash_end)
            state->config.on_splash_end(static_cast<std::uint32_t>(m.args[1]));
        m.result = 0;
        return std::nullopt;
    });
    // void AtlanNetService.MgrPlusDataUrlConnection(String url, int timeout, int id)
    (void)java.register_method(net_class, "MgrPlusDataUrlConnection", "(Ljava/lang/String;II)V",
                               [state](JavaBootstrap::MethodCall& m) -> std::optional<std::string> {
        if (m.args.size() < 3) return "MgrPlusDataUrlConnection: missing arguments";
        HttpRequest request;
        request.url = state->java->string_text(static_cast<std::uint32_t>(m.args[0])).value_or("");
        const auto timeout = static_cast<std::int32_t>(m.args[1]);
        request.timeout_ms = static_cast<int>(timeout <= 0 ? java_timeout_cap_ms : std::min<std::uint64_t>(static_cast<std::uint64_t>(timeout), java_timeout_cap_ms));
        state->request(std::move(request), static_cast<std::uint32_t>(m.args[2]));
        m.result = 0;
        return std::nullopt;
    });
    // void AtlanNetService.MgrPlusPostDataUrlConnection(String url, byte[] data, int timeout, int id)
    (void)java.register_method(net_class, "MgrPlusPostDataUrlConnection", "(Ljava/lang/String;[BII)V",
                               [state](JavaBootstrap::MethodCall& m) -> std::optional<std::string> {
        if (m.args.size() < 4) return "MgrPlusPostDataUrlConnection: missing arguments";
        HttpRequest request;
        request.method = "POST";
        request.url = state->java->string_text(static_cast<std::uint32_t>(m.args[0])).value_or("");
        request.body = state->java->byte_array(static_cast<std::uint32_t>(m.args[1])).value_or(std::vector<std::uint8_t>{});
        request.headers = {{"Content-Type", "application/x-protobuf"}, {"Accept", "application/json"}};
        const auto timeout = static_cast<std::int32_t>(m.args[2]);
        request.timeout_ms = static_cast<int>(timeout <= 0 ? java_timeout_cap_ms : std::min<std::uint64_t>(static_cast<std::uint64_t>(timeout), java_timeout_cap_ms));
        state->request(std::move(request), static_cast<std::uint32_t>(m.args[3]));
        m.result = 0;
        return std::nullopt;
    });
}

std::vector<JavaServices::NativeCall> JavaServices::take_due() {
    auto& state = *state_;
    if (state.java == nullptr) return {};
    // android.net.conn.CONNECTIVITY_CHANGE: the Java side reports the new state once navigation has started.
    if (state.config.use_connection_manager && state.navi_started && state.reported && *state.reported != state.available())
        state.queue_online_state();
    std::deque<State::Reply> replies;
    {
        std::lock_guard lock(state.replies_mutex);
        replies.swap(state.replies);
    }
    if (!replies.empty()) {
        const auto address = state.find_native(net_class, "setResult", "([BII)V");
        if (address == 0) state.log_once("java AtlanNetService.setResult is not registered; replies dropped");
        for (auto& reply : replies) {
            if (address == 0) continue;
            const auto body = state.java->new_byte_array(reply.body);
            if (body == 0) { state.log_once("java: no room for a reply's byte[]"); continue; }
            state.due.push_back({"AtlanNetService.setResult (" + reply.what + ")", address,
                                 {state.java->env(), state.java->class_handle(net_class), body, reply.status, reply.id}});
        }
    }
    std::vector<NativeCall> out(std::make_move_iterator(state.due.begin()), std::make_move_iterator(state.due.end()));
    state.due.clear();
    return out;
}
}
