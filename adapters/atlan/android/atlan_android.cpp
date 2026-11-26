// SPDX-License-Identifier: GPL-2.0-or-later
// adapter: the Atlan app on the Android host. Only what the original phone app does: the guest's surface size goes
// to GL and touch mapping, network comes from the device; its home button (which only the car units' launcher
// answers) stays a logged no-op.
#include "adapters/atlan/atlan_adapter.hpp"
#include "platform/android/android_app.hpp"

namespace atlan::android {
std::shared_ptr<shell::AppAdapter> make_app_adapter(const HostServices& host, const std::string& data_root) {
    (void)data_root;  // the adapter finds ATLAN/ under the session's data root itself
    shell::AtlanAdapter::Config config;
    config.on_set_surfaces = host.set_guest_surface;
    config.http = host.http;
    config.network_available = host.network_available;
    return std::make_shared<shell::AtlanAdapter>(std::move(config));
}
}
