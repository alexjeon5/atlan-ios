// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include "shell/http_client.hpp"
#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace atlan { class JavaBootstrap; }
namespace atlan::shell {
// Host side of the app's Java classes that connect the guest to the network:
//  - MainActivity.IsConnected(): whether a network is available.
//  - MainActivity.SendMsgFromAtlan(): the guest's messages to its Java side. The one that matters here is the
//    navigation start (17): with USECONNECTIONMANAGER set, the Java side answers it with nativeReceiveMsgData(
//    REC_ONLINE_STATE, connected) and repeats that message whenever connectivity changes. The guest stays offline
//    until it gets one. Message 5 is the app's own home button: on the car units the app was made for, the unit's
//    launcher takes it and shows its home screen; the host's on_home stands in for that launcher. Message 13 ends
//    the start-up splash: param 0 hides the splash screen and leaves its sound playing, any other value stops the
//    sound (on_splash_end receives the param).
//  - AtlanNetService.MgrPlusDataUrlConnection / MgrPlusPostDataUrlConnection: an HTTP GET or POST on a background
//    thread, answered through the static native AtlanNetService.setResult(byte[] body, int status, int id) with
//    status 1 (success), 0 (failure) or 4 (malformed URL), as the Java code does.
// Replies the Java side would deliver later (from its handler or its request threads) are queued and handed to
// the session as native calls to make on the guest thread.
class JavaServices {
public:
    struct Config {
        HttpFetch http;                           // nullptr: plain_http_fetch()
        std::function<bool()> network_available;  // nullptr: always available; polled once per take_due()
        bool use_connection_manager{true};        // system.ini USECONNECTIONMANAGER
        std::function<void()> on_home;            // the guest's home button (message 5); guest thread. nullptr: logged only
        std::function<void(std::uint32_t)> on_splash_end;  // message 13 with its param; guest thread. nullptr: logged only
        std::function<void(const std::string&)> log;
    };
    // A guest native method to call: its address and the JNI argument words (env first).
    struct NativeCall {
        std::string what;
        std::uint32_t address{};
        std::vector<std::uint32_t> words;
    };
    // Finds a guest native (registered through RegisterNatives or exported as Java_...); 0 if there is none.
    using FindNative = std::function<std::uint32_t(const std::string& cls, const std::string& name, const std::string& signature)>;
    explicit JavaServices(Config config);
    ~JavaServices();
    JavaServices(const JavaServices&) = delete;
    JavaServices& operator=(const JavaServices&) = delete;
    // Registers the Java methods. `java` must outlive this object's use.
    void install(JavaBootstrap& java, FindNative find_native);
    // Native calls due now, in order. Guest thread only.
    [[nodiscard]] std::vector<NativeCall> take_due();
private:
    struct State;
    std::shared_ptr<State> state_;
};
}
