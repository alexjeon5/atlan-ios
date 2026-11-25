// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include "shell/http_client.hpp"
#include <functional>

namespace atlan {
// HTTP for the guest's Java side: https:// through NSURLSession, http:// over plain sockets (shell/http_client.hpp),
// which App Transport Security does not restrict.
[[nodiscard]] shell::HttpFetch apple_http_fetch();
// Whether the system reports a usable network path (Network framework path monitor; true until its first report).
[[nodiscard]] std::function<bool()> apple_network_available();
}
