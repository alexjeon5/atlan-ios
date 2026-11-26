// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include "shell/app_adapter.hpp"
#include "shell/http_client.hpp"
#include <cstdint>
#include <functional>
#include <memory>
#include <string>

// The seam between the Android host (platform/android: activity, window, GL, audio, input, network) and the one app
// it runs. The host owns everything Android offers; the app (adapters/<app>/android) only turns that into its
// AppAdapter. Exactly one app is linked into the library and defines make_app_adapter.
namespace atlan::android {
struct HostServices {
    shell::HttpFetch http;                     // http:// over host sockets, https:// through java.net
    std::function<bool()> network_available;   // the device's default network
    // The guest chose its surface size (for example through its Java side's surface callback): GL renders at that
    // size and touches map to it. Guest thread.
    std::function<void(std::int32_t width, std::int32_t height)> set_guest_surface;
};

// The adapter for one session. `data_root` is the folder the activity chose (where the app's data folder lies).
[[nodiscard]] std::shared_ptr<shell::AppAdapter> make_app_adapter(const HostServices& host, const std::string& data_root);
}
