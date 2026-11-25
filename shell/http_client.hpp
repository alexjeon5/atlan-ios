// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include <cstdint>
#include <functional>
#include <string>
#include <utility>
#include <vector>

namespace atlan::shell {
// One HTTP request the guest's Java side would make through java.net.HttpURLConnection.
struct HttpRequest {
    std::string method{"GET"};
    std::string url;
    std::vector<std::pair<std::string, std::string>> headers;
    std::vector<std::uint8_t> body;
    int timeout_ms{10000};  // connect and read, each
};
struct HttpResponse {
    enum class Outcome : std::uint8_t { ok, transport_error, malformed_url };
    Outcome outcome{Outcome::transport_error};
    int status{};  // HTTP status when outcome is ok
    std::vector<std::uint8_t> body;
    std::string error;  // why, when outcome is not ok
};
// Asynchronous fetch: `done` is called exactly once, on any thread.
using HttpFetch = std::function<void(HttpRequest, std::function<void(HttpResponse)>)>;

// Plain http:// over host sockets (redirects followed, chunked bodies decoded). Blocks the calling thread.
// https:// URLs fail with a transport error: they need the platform's client.
[[nodiscard]] HttpResponse fetch_plain_http(const HttpRequest& request);
// fetch_plain_http on a detached host thread per request.
[[nodiscard]] HttpFetch plain_http_fetch();
// The URL without its query and fragment, for logs: query strings carry keys and personal values.
[[nodiscard]] std::string url_for_log(const std::string& url);
}
