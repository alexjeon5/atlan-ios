// SPDX-License-Identifier: GPL-2.0-or-later
#include "shell/http_client.hpp"
#include <algorithm>
#include <cctype>
#include <cerrno>
#include <chrono>
#include <cstring>
#include <fcntl.h>
#include <netdb.h>
#include <optional>
#include <poll.h>
#include <sys/socket.h>
#include <thread>
#include <unistd.h>

namespace atlan::shell {
namespace {
using Clock = std::chrono::steady_clock;
constexpr std::size_t max_body = 32U << 20;
constexpr std::size_t max_head = 64U << 10;
constexpr int max_redirects = 5;

struct Url { std::string host, port{"80"}, target{"/"}; };
std::optional<Url> parse(const std::string& url, bool& https) {
    const auto scheme_end = url.find("://");
    if (scheme_end == std::string::npos) return std::nullopt;
    std::string scheme = url.substr(0, scheme_end);
    std::transform(scheme.begin(), scheme.end(), scheme.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    https = scheme == "https";
    if (scheme != "http" && !https) return std::nullopt;
    const auto rest = url.substr(scheme_end + 3);
    const auto slash = rest.find_first_of("/?#");
    Url out;
    std::string authority = rest.substr(0, slash);
    if (slash != std::string::npos) {
        out.target = rest.substr(slash);
        if (out.target[0] != '/') out.target.insert(0, "/");
        if (const auto hash = out.target.find('#'); hash != std::string::npos) out.target.resize(hash);
    }
    if (const auto at = authority.rfind('@'); at != std::string::npos) authority.erase(0, at + 1);
    if (const auto colon = authority.rfind(':'); colon != std::string::npos && authority.find(']') == std::string::npos) {
        out.port = authority.substr(colon + 1);
        authority.resize(colon);
        if (out.port.empty() || !std::all_of(out.port.begin(), out.port.end(), [](unsigned char c) { return std::isdigit(c); })) return std::nullopt;
    }
    if (authority.size() > 2 && authority.front() == '[' && authority.back() == ']') authority = authority.substr(1, authority.size() - 2);
    if (authority.empty() || authority.find_first_of(" \t\r\n") != std::string::npos) return std::nullopt;
    out.host = authority;
    // Spaces and control characters cannot go on the request line; Java's URL class would send them escaped.
    std::string target;
    for (unsigned char c : out.target) {
        if (c <= 0x20 || c >= 0x7f) { static const char* hex = "0123456789ABCDEF"; target += '%'; target += hex[c >> 4]; target += hex[c & 15]; }
        else target += static_cast<char>(c);
    }
    out.target = std::move(target);
    return out;
}
int milliseconds_left(Clock::time_point deadline) {
    const auto left = std::chrono::duration_cast<std::chrono::milliseconds>(deadline - Clock::now()).count();
    return left <= 0 ? 0 : static_cast<int>(std::min<long long>(left, 1 << 30));
}
// Connected non-blocking socket, or -1 with `error` set.
int open_connection(const Url& url, int timeout_ms, std::string& error) {
    addrinfo hints{};
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
#ifdef AI_DEFAULT
    hints.ai_flags = AI_DEFAULT;
#endif
    addrinfo* found = nullptr;
    if (const int status = ::getaddrinfo(url.host.c_str(), url.port.c_str(), &hints, &found); status != 0) {
        error = std::string("lookup failed: ") + gai_strerror(status);
        return -1;
    }
    const auto deadline = Clock::now() + std::chrono::milliseconds(timeout_ms);
    int fd = -1;
    for (auto* a = found; a != nullptr && fd < 0; a = a->ai_next) {
        fd = ::socket(a->ai_family, a->ai_socktype, a->ai_protocol);
        if (fd < 0) continue;
        ::fcntl(fd, F_SETFL, ::fcntl(fd, F_GETFL, 0) | O_NONBLOCK);
#ifdef SO_NOSIGPIPE
        const int on = 1;
        ::setsockopt(fd, SOL_SOCKET, SO_NOSIGPIPE, &on, sizeof on);
#endif
        int status = ::connect(fd, a->ai_addr, a->ai_addrlen);
        if (status != 0 && errno == EINPROGRESS) {
            pollfd p{fd, POLLOUT, 0};
            if (::poll(&p, 1, milliseconds_left(deadline)) > 0) {
                int so_error = 0;
                socklen_t length = sizeof so_error;
                ::getsockopt(fd, SOL_SOCKET, SO_ERROR, &so_error, &length);
                status = so_error == 0 ? 0 : -1;
                if (so_error != 0) error = std::string("connect failed: ") + std::strerror(so_error);
            } else {
                error = "connect timed out";
            }
        } else if (status != 0) {
            error = std::string("connect failed: ") + std::strerror(errno);
        }
        if (status != 0) { ::close(fd); fd = -1; }
    }
    ::freeaddrinfo(found);
    if (fd >= 0) error.clear();
    return fd;
}
#ifdef MSG_NOSIGNAL
constexpr int send_flags = MSG_NOSIGNAL;
#else
constexpr int send_flags = 0;
#endif
bool send_all(int fd, const char* data, std::size_t size, int timeout_ms) {
    while (size != 0) {
        const auto n = ::send(fd, data, size, send_flags);
        if (n > 0) { data += n; size -= static_cast<std::size_t>(n); continue; }
        if (n < 0 && errno != EAGAIN && errno != EWOULDBLOCK && errno != EINTR) return false;
        pollfd p{fd, POLLOUT, 0};
        if (::poll(&p, 1, timeout_ms) <= 0) return false;
    }
    return true;
}
// Reads until the peer closes or `enough` says the response is complete. False on a read error or timeout.
bool receive(int fd, int timeout_ms, std::string& data, const std::function<bool(const std::string&)>& enough) {
    char buffer[16384];
    while (!enough(data)) {
        const auto n = ::recv(fd, buffer, sizeof buffer, 0);
        if (n > 0) {
            data.append(buffer, static_cast<std::size_t>(n));
            if (data.size() > max_body + max_head) return false;
            continue;
        }
        if (n == 0) return true;
        if (errno != EAGAIN && errno != EWOULDBLOCK && errno != EINTR) return false;
        pollfd p{fd, POLLIN, 0};
        if (::poll(&p, 1, timeout_ms) <= 0) return false;  // a read timeout, as HttpURLConnection's
    }
    return true;
}
std::string lower(std::string text) {
    std::transform(text.begin(), text.end(), text.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return text;
}
std::string trim(const std::string& text) {
    const auto a = text.find_first_not_of(" \t");
    if (a == std::string::npos) return {};
    return text.substr(a, text.find_last_not_of(" \t\r") - a + 1);
}
// Decodes a chunked body; nullopt while incomplete or malformed (`complete` tells which).
std::optional<std::string> dechunk(const std::string& raw, bool& complete) {
    std::string out;
    std::size_t at = 0;
    complete = false;
    while (true) {
        const auto line_end = raw.find("\r\n", at);
        if (line_end == std::string::npos) return std::nullopt;
        std::size_t size = 0;
        try { size = std::stoul(raw.substr(at, line_end - at), nullptr, 16); } catch (...) { complete = true; return std::nullopt; }
        at = line_end + 2;
        if (size == 0) { complete = true; return out; }
        if (raw.size() < at + size + 2) return std::nullopt;
        out.append(raw, at, size);
        at += size + 2;
    }
}
}

std::string url_for_log(const std::string& url) {
    return url.substr(0, url.find_first_of("?#"));
}

HttpResponse fetch_plain_http(const HttpRequest& original) {
    HttpRequest request = original;
    HttpResponse response;
    const int timeout = std::max(request.timeout_ms, 1);
    for (int redirect = 0;; ++redirect) {
        bool https = false;
        const auto url = parse(request.url, https);
        if (!url) { response.outcome = HttpResponse::Outcome::malformed_url; response.error = "malformed URL"; return response; }
        if (https) { response.error = "https needs the platform client"; return response; }
        const int fd = open_connection(*url, timeout, response.error);
        if (fd < 0) return response;
        std::string head = request.method + " " + url->target + " HTTP/1.1\r\nHost: " + url->host +
                           (url->port == "80" ? "" : ":" + url->port) + "\r\nConnection: close\r\nAccept-Encoding: identity\r\n";
        bool has_agent = false;
        for (const auto& [name, value] : request.headers) {
            head += name + ": " + value + "\r\n";
            has_agent = has_agent || lower(name) == "user-agent";
        }
        if (!has_agent) head += "User-Agent: Dalvik/2.1.0 (Linux; U; Android)\r\n";
        if (request.method != "GET" || !request.body.empty()) head += "Content-Length: " + std::to_string(request.body.size()) + "\r\n";
        head += "\r\n";
        if (!send_all(fd, head.data(), head.size(), timeout) ||
            (!request.body.empty() && !send_all(fd, reinterpret_cast<const char*>(request.body.data()), request.body.size(), timeout))) {
            ::close(fd);
            response.error = "send failed";
            return response;
        }
        std::string data;
        std::optional<std::size_t> header_end, content_length;
        bool chunked = false;
        const bool received = receive(fd, timeout, data, [&](const std::string& d) {
            if (!header_end) {
                const auto end = d.find("\r\n\r\n");
                if (end == std::string::npos) return d.size() > max_head;
                header_end = end + 4;
                const std::string headers = lower(d.substr(0, end));
                if (const auto at = headers.find("\r\ncontent-length:"); at != std::string::npos) {
                    try { content_length = std::stoull(headers.substr(at + 17, headers.find("\r\n", at + 2) - at - 17)); } catch (...) {}
                }
                chunked = headers.find("\r\ntransfer-encoding: chunked") != std::string::npos ||
                          headers.find("\r\ntransfer-encoding:chunked") != std::string::npos;
            }
            if (content_length) return d.size() >= *header_end + *content_length;
            if (chunked) { bool complete = false; (void)dechunk(d.substr(*header_end), complete); return complete; }
            return false;
        });
        ::close(fd);
        if (!header_end) { response.error = received ? "no response header" : "read failed"; return response; }
        // Status line: HTTP/1.x NNN reason
        const std::string status_line = data.substr(0, data.find("\r\n"));
        const auto space = status_line.find(' ');
        int status = 0;
        try { status = space == std::string::npos ? 0 : std::stoi(status_line.substr(space + 1, 3)); } catch (...) {}
        if (status < 100) { response.error = "malformed status line"; return response; }
        std::string body = data.substr(*header_end);
        if (chunked) {
            bool complete = false;
            auto decoded = dechunk(body, complete);
            if (!decoded) { response.error = "incomplete chunked body"; return response; }
            body = std::move(*decoded);
        } else if (content_length) {
            if (body.size() < *content_length) { response.error = received ? "body shorter than Content-Length" : "read failed"; return response; }
            body.resize(*content_length);
        } else if (!received) {
            response.error = "read failed";
            return response;
        }
        if ((status == 301 || status == 302 || status == 303 || status == 307 || status == 308) && redirect < max_redirects) {
            const std::string headers = data.substr(0, *header_end);
            const auto at = lower(headers).find("\r\nlocation:");
            if (at != std::string::npos) {
                std::string location = trim(headers.substr(at + 11, headers.find("\r\n", at + 2) - at - 11));
                if (!location.empty() && location[0] == '/') location = "http://" + url->host + (url->port == "80" ? "" : ":" + url->port) + location;
                // HttpURLConnection does not follow a redirect to another protocol.
                if (location.rfind("http://", 0) == 0) {
                    request.url = location;
                    if (status == 303 || ((status == 301 || status == 302) && request.method == "POST")) { request.method = "GET"; request.body.clear(); }
                    continue;
                }
            }
        }
        response.outcome = HttpResponse::Outcome::ok;
        response.status = status;
        response.body.assign(body.begin(), body.end());
        response.error.clear();
        return response;
    }
}

HttpFetch plain_http_fetch() {
    return [](HttpRequest request, std::function<void(HttpResponse)> done) {
        std::thread([request = std::move(request), done = std::move(done)] { done(fetch_plain_http(request)); }).detach();
    };
}
}
