// SPDX-License-Identifier: GPL-2.0-or-later
// Guest sockets (atlan/sockets.hpp) and the plain HTTP client (shell/http_client.hpp) against loopback servers, and
// sscanf (atlan/scanf.hpp), which the guest's online code is the first to use.
#include "atlan/scanf.hpp"
#include "atlan/sockets.hpp"
#include "shell/http_client.hpp"
#include <arpa/inet.h>
#include <atomic>
#include <cstring>
#include <iostream>
#include <netinet/in.h>
#include <stdexcept>
#include <string>
#include <sys/socket.h>
#include <thread>
#include <unistd.h>

namespace {
unsigned checks{};
void check(bool result, const char* message) {
    ++checks;
    if (!result) throw std::runtime_error(message);
}
// A loopback TCP server that answers each connection with `respond(request)` and closes it.
class Server {
public:
    explicit Server(std::function<std::string(const std::string&)> respond) : respond_(std::move(respond)) {
        fd_ = ::socket(AF_INET, SOCK_STREAM, 0);
        sockaddr_in address{};
        address.sin_family = AF_INET;
        address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        if (::bind(fd_, reinterpret_cast<sockaddr*>(&address), sizeof address) != 0 || ::listen(fd_, 8) != 0)
            throw std::runtime_error("loopback listen failed");
        socklen_t length = sizeof address;
        ::getsockname(fd_, reinterpret_cast<sockaddr*>(&address), &length);
        port_ = ntohs(address.sin_port);
        thread_ = std::thread([this] {
            while (true) {
                const int client = ::accept(fd_, nullptr, nullptr);
                if (client < 0 || stop_.load()) { if (client >= 0) ::close(client); return; }
                std::string request;
                char buffer[4096];
                // Read the head, then a body of Content-Length bytes.
                while (request.find("\r\n\r\n") == std::string::npos) {
                    const auto n = ::recv(client, buffer, sizeof buffer, 0);
                    if (n <= 0) break;
                    request.append(buffer, static_cast<std::size_t>(n));
                }
                if (const auto at = request.find("Content-Length: "); at != std::string::npos) {
                    const auto size = std::stoul(request.substr(at + 16));
                    while (request.size() < request.find("\r\n\r\n") + 4 + size) {
                        const auto n = ::recv(client, buffer, sizeof buffer, 0);
                        if (n <= 0) break;
                        request.append(buffer, static_cast<std::size_t>(n));
                    }
                }
                last_request = request;
                const std::string reply = respond_(request);
                ::send(client, reply.data(), reply.size(), 0);
                ::close(client);
            }
        });
    }
    ~Server() {
        stop_.store(true);
        const int poke = ::socket(AF_INET, SOCK_STREAM, 0);
        sockaddr_in address{};
        address.sin_family = AF_INET;
        address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        address.sin_port = htons(port_);
        ::connect(poke, reinterpret_cast<sockaddr*>(&address), sizeof address);
        ::close(poke);
        thread_.join();
        ::close(fd_);
    }
    [[nodiscard]] std::uint16_t port() const { return port_; }
    std::string last_request;
private:
    std::function<std::string(const std::string&)> respond_;
    int fd_{-1};
    std::uint16_t port_{};
    std::atomic<bool> stop_{false};
    std::thread thread_;
};

void http_tests() {
    using atlan::shell::HttpRequest;
    using atlan::shell::HttpResponse;
    using atlan::shell::fetch_plain_http;
    {
        Server server([](const std::string&) { return std::string("HTTP/1.1 200 OK\r\nContent-Length: 5\r\nConnection: close\r\n\r\nhello"); });
        HttpRequest request;
        request.url = "http://127.0.0.1:" + std::to_string(server.port()) + "/a b?x=1#frag";
        const auto response = fetch_plain_http(request);
        check(response.outcome == HttpResponse::Outcome::ok && response.status == 200, "GET succeeds");
        check(std::string(response.body.begin(), response.body.end()) == "hello", "Content-Length body");
        check(server.last_request.rfind("GET /a%20b?x=1 HTTP/1.1\r\n", 0) == 0, "request line escapes spaces and drops the fragment");
        check(server.last_request.find("Host: 127.0.0.1:" + std::to_string(server.port()) + "\r\n") != std::string::npos, "Host header");
    }
    {
        Server server([](const std::string&) {
            return std::string("HTTP/1.1 200 OK\r\nTransfer-Encoding: chunked\r\n\r\n4\r\nWiki\r\n5\r\npedia\r\n0\r\n\r\n");
        });
        HttpRequest request;
        request.method = "POST";
        request.url = "http://127.0.0.1:" + std::to_string(server.port()) + "/post";
        request.body = {1, 2, 3};
        request.headers = {{"Content-Type", "application/x-protobuf"}};
        const auto response = fetch_plain_http(request);
        check(response.outcome == HttpResponse::Outcome::ok && std::string(response.body.begin(), response.body.end()) == "Wikipedia",
              "chunked body is decoded");
        check(server.last_request.find("Content-Length: 3\r\n") != std::string::npos &&
              server.last_request.find("Content-Type: application/x-protobuf\r\n") != std::string::npos &&
              server.last_request.substr(server.last_request.size() - 3) == std::string("\x01\x02\x03", 3), "POST body and headers");
    }
    {
        Server target([](const std::string&) { return std::string("HTTP/1.0 404 Not Found\r\n\r\nmissing"); });
        const std::string location = "http://127.0.0.1:" + std::to_string(target.port()) + "/moved";
        Server server([location](const std::string&) { return "HTTP/1.1 302 Found\r\nLocation: " + location + "\r\nContent-Length: 0\r\n\r\n"; });
        HttpRequest request;
        request.url = "http://127.0.0.1:" + std::to_string(server.port()) + "/";
        const auto response = fetch_plain_http(request);
        check(response.outcome == HttpResponse::Outcome::ok && response.status == 404, "redirect followed; status passed through");
        check(std::string(response.body.begin(), response.body.end()) == "missing", "body read to the end without a length");
    }
    {
        HttpRequest request;
        request.url = "not a url";
        check(fetch_plain_http(request).outcome == HttpResponse::Outcome::malformed_url, "malformed URL");
        request.url = "http://127.0.0.1:1/";
        request.timeout_ms = 2000;
        check(fetch_plain_http(request).outcome == HttpResponse::Outcome::transport_error, "refused connection is a transport error");
        check(atlan::shell::url_for_log("http://h/p/q?key=secret#x") == "http://h/p/q", "logged URL has no query");
    }
}

// Calls one bound service with word arguments (stack words beyond the fourth at `stack`).
std::uint32_t call(atlan::ImportDispatcher& dispatcher, atlan::Runtime& runtime, const char* name,
                   std::initializer_list<std::uint32_t> words, std::uint32_t stack = 0x30000) {
    const auto* service = dispatcher.service(name);
    if (service == nullptr) throw std::runtime_error(std::string("unbound ") + name);
    unsigned i = 0;
    for (auto w : words) {
        if (i < 4) runtime.cpu.registers[i] = w;
        else {
            std::array<std::byte, 4> b{};
            for (unsigned k = 0; k < 4; ++k) b[k] = std::byte((w >> (8 * k)) & 255);
            if (!runtime.memory.write(stack + 4 * (i - 4), b)) throw std::runtime_error("stack write");
        }
        ++i;
    }
    runtime.cpu.registers[13] = stack;
    atlan::ImportCall c(dispatcher, runtime, name);
    if (auto error = (*service)(c)) throw std::runtime_error(*error);
    return static_cast<std::uint32_t>(runtime.cpu.registers[0]);
}
std::uint32_t read32(atlan::Runtime& runtime, std::uint32_t at) {
    std::array<std::byte, 4> b{};
    if (!runtime.memory.read(at, b)) throw std::runtime_error("read");
    std::uint32_t v = 0;
    for (unsigned k = 0; k < 4; ++k) v |= std::to_integer<std::uint32_t>(b[k]) << (8 * k);
    return v;
}
void write_bytes(atlan::Runtime& runtime, std::uint32_t at, const std::string& text) {
    if (!runtime.memory.write(at, std::span<const std::byte>(reinterpret_cast<const std::byte*>(text.data()), text.size())))
        throw std::runtime_error("write");
}

void socket_tests() {
    Server server([](const std::string& request) { return "echo:" + request; });
    atlan::Runtime runtime;
    runtime.memory.map(0x10000, 0x30000, atlan::Permissions::read_write);
    atlan::ImportDispatcher dispatcher;
    bool other_close = false;
    dispatcher.bind("close", [&](atlan::ImportCall& c) -> std::optional<std::string> { other_close = true; c.set_result(0); return std::nullopt; });
    std::vector<std::string> trace;
    atlan::GuestSockets::Config config;
    config.errno_address = 0x10000;
    config.trace = [&](const std::string& line) { trace.push_back(line); };
    atlan::GuestSockets sockets(config);
    sockets.install(dispatcher);

    // gethostbyname on a literal: hostent {name, aliases, type, length, list} in the data page.
    write_bytes(runtime, 0x11000, std::string("127.0.0.1") + '\0');
    const auto host = call(dispatcher, runtime, "gethostbyname", {0x11000});
    check(host == config.data_page && read32(runtime, host + 8) == 2 && read32(runtime, host + 12) == 4, "hostent fields");
    const auto first = read32(runtime, read32(runtime, host + 16));
    check(read32(runtime, first) == htonl(INADDR_LOOPBACK) && read32(runtime, read32(runtime, host + 16) + 4) == 0, "address list");
    write_bytes(runtime, 0x11000, std::string("localhost") + '\0');
    check(call(dispatcher, runtime, "gethostbyname", {0x11000}) == config.data_page, "name lookup on a host thread");

    const auto fd = call(dispatcher, runtime, "socket", {2, 1, 0});
    check(fd >= atlan::GuestSockets::first_descriptor, "socket descriptor range");
    // getsockopt(SOL_SOCKET, SO_RCVBUF) then setsockopt(SO_RCVTIMEO, 2 s) as the guest does.
    write_bytes(runtime, 0x12004, std::string("\x04\0\0\0", 4));
    check(call(dispatcher, runtime, "getsockopt", {fd, 1, 8, 0x12000, 0x12004}) == 0 && read32(runtime, 0x12000) > 0, "SO_RCVBUF read");
    write_bytes(runtime, 0x12010, std::string("\x02\0\0\0\0\0\0\0", 8));
    check(call(dispatcher, runtime, "setsockopt", {fd, 1, 20, 0x12010, 8}) == 0, "SO_RCVTIMEO set");
    // sockaddr_in: family 2 (little endian), port and address in network order.
    std::string address(16, '\0');
    address[0] = 2;
    address[2] = static_cast<char>(server.port() >> 8);
    address[3] = static_cast<char>(server.port() & 255);
    address[4] = 127; address[7] = 1;
    write_bytes(runtime, 0x13000, address);
    check(call(dispatcher, runtime, "connect", {fd, 0x13000, 16}) == 0, "connect");
    write_bytes(runtime, 0x14000, "GET / HTTP/1.1\r\n\r\n");
    check(call(dispatcher, runtime, "write", {fd, 0x14000, 18}) == 18, "write on a socket sends");
    std::string received;
    while (true) {
        const auto n = call(dispatcher, runtime, "recv", {fd, 0x15000, 256, 0});
        check(n != 0xffffffffU, "recv");
        if (n == 0) break;
        std::string part(n, '\0');
        std::array<std::byte, 256> raw{};
        (void)runtime.memory.read(0x15000, std::span<std::byte>(raw.data(), n));
        std::memcpy(part.data(), raw.data(), n);
        received += part;
    }
    check(received == "echo:GET / HTTP/1.1\r\n\r\n", "echoed data");
    check(call(dispatcher, runtime, "close", {fd}) == 0 && !other_close && sockets.open_sockets() == 0, "socket close stays here");
    check(call(dispatcher, runtime, "close", {5}) == 0 && other_close, "other descriptors go to the earlier close");
    check(call(dispatcher, runtime, "send", {fd, 0x14000, 1, 0}) == 0xffffffffU && read32(runtime, 0x10000) == 9, "closed socket is EBADF");
    // A refused connection reports ECONNREFUSED.
    const auto refused = call(dispatcher, runtime, "socket", {2, 1, 0});
    address[2] = 0; address[3] = 1;
    write_bytes(runtime, 0x13000, address);
    check(call(dispatcher, runtime, "connect", {refused, 0x13000, 16}) == 0xffffffffU && read32(runtime, 0x10000) == 111, "ECONNREFUSED");
    check(call(dispatcher, runtime, "socket", {10, 1, 0}) == 0xffffffffU && read32(runtime, 0x10000) == 97, "IPv6 sockets unsupported");
    bool traced_connect = false;
    for (const auto& line : trace) traced_connect = traced_connect || line.rfind("net connect 127.0.0.1:", 0) == 0;
    check(traced_connect, "connections are traced");
}

void scanf_tests() {
    atlan::Runtime runtime;
    runtime.memory.map(0x10000, 0x30000, atlan::Permissions::read_write);
    atlan::ImportDispatcher dispatcher;
    atlan::install_scanf_services(dispatcher);
    write_bytes(runtime, 0x11000, std::string("HTTP/1.1 200 OK") + '\0');
    write_bytes(runtime, 0x11100, std::string("HTTP/%d.%d %d") + '\0');
    check(call(dispatcher, runtime, "sscanf", {0x11000, 0x11100, 0x12000, 0x12004, 0x12008}) == 3 &&
          read32(runtime, 0x12000) == 1 && read32(runtime, 0x12004) == 1 && read32(runtime, 0x12008) == 200, "sscanf integers");
    write_bytes(runtime, 0x11000, std::string("  name ff -2.5 abc,def|x") + '\0');
    write_bytes(runtime, 0x11100, std::string("%s %x %lf %[^,],%*[a-z]|%c%n") + '\0');
    check(call(dispatcher, runtime, "sscanf", {0x11000, 0x11100, 0x13000, 0x12000, 0x12010, 0x13100, 0x12020, 0x12024}) == 5, "sscanf mixed count");
    std::array<std::byte, 8> raw{};
    (void)runtime.memory.read(0x12010, raw);
    double value = 0;
    std::memcpy(&value, raw.data(), 8);
    std::array<std::byte, 5> name{}, set{};
    (void)runtime.memory.read(0x13000, name);
    (void)runtime.memory.read(0x13100, set);
    check(read32(runtime, 0x12000) == 0xff && value == -2.5 && std::memcmp(name.data(), "name", 5) == 0 &&
          std::memcmp(set.data(), "abc", 4) == 0 && (read32(runtime, 0x12020) & 0xff) == 'x' && read32(runtime, 0x12024) == 24,
          "sscanf conversions");
    write_bytes(runtime, 0x11000, std::string("   ") + '\0');
    write_bytes(runtime, 0x11100, std::string("%d") + '\0');
    check(call(dispatcher, runtime, "sscanf", {0x11000, 0x11100, 0x12000}) == 0xffffffffU, "sscanf on empty input is EOF");
    write_bytes(runtime, 0x11000, std::string("x1") + '\0');
    check(call(dispatcher, runtime, "sscanf", {0x11000, 0x11100, 0x12000}) == 0, "sscanf matching failure");
}
}

int main() {
    try {
        http_tests();
        socket_tests();
        scanf_tests();
    } catch (const std::exception& error) {
        std::cerr << "network tests failed after " << checks << " checks: " << error.what() << '\n';
        return 1;
    }
    std::cout << "network tests: " << checks << " checks passed\n";
    return 0;
}
