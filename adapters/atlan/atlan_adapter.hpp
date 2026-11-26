// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include "shell/app_adapter.hpp"
#include "shell/http_client.hpp"
#include <cstdint>
#include <functional>
#include <memory>
#include <string>

// The Atlan app: its package in the data folder, its settings file and folder aliases, its Java side (surface size,
// GL thread frame requests, network services, logged UI callbacks) and its native start and frame entry points.
namespace atlan::shell {
class JavaServices;
class AtlanAdapter final : public AppAdapter {
public:
    struct Config {
        // Called on the guest thread when the guest reports MainActivity.setSurfaces(a, b). Arguments are passed
        // through unchanged; the embedder decides what they mean.
        std::function<void(std::int32_t, std::int32_t)> on_set_surfaces;
        // Called on the guest thread when the user presses the app's own home button. The car units the app was
        // made for answer it with their launcher's home screen; the embedder shows its own. nullptr: logged only.
        std::function<void()> on_home;
        // Network for the Java side (shell/java_services.hpp): HTTP requests (nullptr: plain http:// only) and
        // whether a network is available (nullptr: always). Called from host threads and the guest thread.
        HttpFetch http;
        std::function<bool()> network_available;
        // The ATLAN folder when it lies outside the data root (a folder the user picked elsewhere), read in place.
        // What the app writes stays in the data root's ATLAN/NAND. Empty: the data root's ATLAN folder.
        std::string map_root;
    };
    AtlanAdapter();
    explicit AtlanAdapter(Config config);
    ~AtlanAdapter() override;

    [[nodiscard]] std::string find_library(const std::string& data_root) const override;
    [[nodiscard]] std::string missing_library() const override;
    void unpack_library(std::vector<std::byte>& bytes, const SessionLog& log) override;
    void configure_files(GuestStdio& stdio, const SessionLog& log) override;
    void install_java(AppHost& host, bool frame_requests) override;
    void uninstall_java() override;
    [[nodiscard]] AppStartup startup() const override;
    [[nodiscard]] std::vector<NativeSet> native_sets() const override;
    bool deliver(AppHost& host) override;
    [[nodiscard]] bool frames_on_request() const override { return on_demand_; }
    [[nodiscard]] bool frame_requested() const override { return render_requested_; }
    void frame_drawn() override { render_requested_ = false; }
    std::uint64_t take_frame_request_count() override;
    std::optional<bool> frame_ready(AppHost& host) override;

private:
    Config config_;
    bool use_connection_manager_{true};  // system.ini USECONNECTIONMANAGER (the stock value is 1)
    std::unique_ptr<JavaServices> java_services_;
    // AtlanGLThread natives behind the frame request protocol; all three or on-demand frames are off.
    std::uint32_t is_state_ready_{}, is_gl_init_{}, set_state_{};
    bool on_demand_{};
    bool render_requested_{};
    std::uint64_t render_requests_{};
};
}
