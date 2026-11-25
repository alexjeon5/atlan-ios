// SPDX-License-Identifier: GPL-2.0-or-later
#include "atlan/sockets.hpp"
#include "atlan/abi.hpp"
#include "atlan/threads.hpp"
#include <algorithm>
#include <arpa/inet.h>
#include <array>
#include <atomic>
#include <cerrno>
#include <cstring>
#include <fcntl.h>
#include <mutex>
#include <netdb.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <optional>
#include <poll.h>
#include <sys/socket.h>
#include <thread>
#include <unistd.h>
#include <vector>

namespace atlan {
namespace {
using Clock = std::chrono::steady_clock;
// ARM Linux values the guest uses.
constexpr std::uint32_t linux_af_inet = 2, linux_sock_stream = 1, linux_sock_dgram = 2;
constexpr std::uint32_t linux_sock_type_mask = 0xf, linux_sock_nonblock = 04000;
constexpr std::uint32_t linux_sol_socket = 1, linux_ipproto_tcp = 6;
constexpr std::uint32_t linux_so_reuseaddr = 2, linux_so_error = 4, linux_so_sndbuf = 7, linux_so_rcvbuf = 8,
                        linux_so_keepalive = 9, linux_so_linger = 13, linux_so_rcvtimeo = 20, linux_so_sndtimeo = 21;
constexpr std::uint32_t linux_tcp_nodelay = 1;
constexpr std::uint32_t linux_msg_peek = 2, linux_msg_dontwait = 0x40;
constexpr std::uint32_t e_badf = 9, e_again = 11, e_inval = 22, e_afnosupport = 97, e_timedout = 110, e_inprogress = 115,
                        e_isconn = 106, e_notconn = 107, e_io = 5, e_mfile = 24;
constexpr auto retry_interval = std::chrono::milliseconds(5);
constexpr std::size_t max_io = 1U << 20;
constexpr std::size_t max_sockets = 256;

std::uint32_t guest_errno(int host) {
    switch (host) {
    case EAGAIN: return e_again;
#if EWOULDBLOCK != EAGAIN
    case EWOULDBLOCK: return e_again;
#endif
    case EINTR: return 4;
    case EBADF: return e_badf;
    case ENOMEM: return 12;
    case EACCES: return 13;
    case EINVAL: return e_inval;
    case EMFILE: return e_mfile;
    case EPIPE: return 32;
    case EAFNOSUPPORT: return e_afnosupport;
    case EADDRINUSE: return 98;
    case EADDRNOTAVAIL: return 99;
    case ENETDOWN: return 100;
    case ENETUNREACH: return 101;
    case ECONNABORTED: return 103;
    case ECONNRESET: return 104;
    case ENOBUFS: return 105;
    case EISCONN: return e_isconn;
    case ENOTCONN: return e_notconn;
    case ETIMEDOUT: return e_timedout;
    case ECONNREFUSED: return 111;
    case EHOSTDOWN: return 112;
    case EHOSTUNREACH: return 113;
    case EALREADY: return 114;
    case EINPROGRESS: return e_inprogress;
    default: return e_io;
    }
}
std::string ipv4_text(std::uint32_t network_order) {
    char text[INET_ADDRSTRLEN] = {};
    in_addr address{};
    address.s_addr = network_order;
    inet_ntop(AF_INET, &address, text, sizeof text);
    return text;
}
std::uint32_t le32(const std::byte* p) {
    return std::to_integer<std::uint32_t>(p[0]) | std::to_integer<std::uint32_t>(p[1]) << 8 |
           std::to_integer<std::uint32_t>(p[2]) << 16 | std::to_integer<std::uint32_t>(p[3]) << 24;
}
void put32(std::byte* p, std::uint32_t value) {
    for (unsigned i = 0; i < 4; ++i) p[i] = std::byte((value >> (8 * i)) & 255U);
}
void configure(int fd) {
    const int flags = ::fcntl(fd, F_GETFL, 0);
    ::fcntl(fd, F_SETFL, flags | O_NONBLOCK);
    ::fcntl(fd, F_SETFD, FD_CLOEXEC);
#ifdef SO_NOSIGPIPE
    const int on = 1;
    ::setsockopt(fd, SOL_SOCKET, SO_NOSIGPIPE, &on, sizeof on);
#endif
}
#ifdef MSG_NOSIGNAL
constexpr int send_flags = MSG_NOSIGNAL;
#else
constexpr int send_flags = 0;
#endif
}

struct GuestSockets::State {
    Config config;
    struct Socket {
        int host{-1};
        int type{SOCK_STREAM};
        bool guest_nonblocking{};
        bool connected{};
        std::uint32_t receive_timeout_ms{}, send_timeout_ms{};  // 0: none
        // Options the guest set, applied again when the host socket is replaced by one of another address family.
        std::optional<int> receive_buffer, send_buffer, no_delay, keep_alive;
        // Deadlines of an operation that is waiting (the guest thread retries the same call until it is done).
        std::optional<Clock::time_point> connect_deadline, receive_deadline, send_deadline;
        std::string peer;  // host:port, for the trace
        std::uint64_t sent{}, received{};
    };
    std::vector<Socket> sockets;
    // Name lookups: running or finished, shared with the host thread that resolves them.
    struct Lookup {
        std::atomic<bool> done{false};
        std::vector<std::uint32_t> addresses;  // IPv4, network order
        std::string error;
        Clock::time_point started{Clock::now()}, finished{};
    };
    std::mutex lookups_mutex;
    std::map<std::string, std::shared_ptr<Lookup>> lookups;
    ImportService previous_close;

    void trace(const std::string& line) const { if (config.trace) config.trace("net " + line); }
    Socket* socket(std::uint32_t fd) {
        if (fd < first_descriptor || fd - first_descriptor >= sockets.size()) return nullptr;
        auto& entry = sockets[fd - first_descriptor];
        return entry.host < 0 ? nullptr : &entry;
    }
    bool set_errno(ImportCall& call, std::uint32_t code) const {
        if (config.errno_address == 0) return true;
        std::array<std::byte, 4> bytes{};
        put32(bytes.data(), code);
        return call.runtime().memory.write(config.errno_address, bytes);
    }
    std::optional<std::string> fail(ImportCall& call, std::uint32_t code, const char* what) const {
        if (!set_errno(call, code)) return std::string(what) + " could not write errno";
        call.set_result(0xffffffffU);
        return std::nullopt;
    }
    // Whether the calling guest thread can be blocked (and retried) instead of waiting on the host.
    bool can_block(ImportCall& call) const { return config.threads != nullptr && call.dispatcher().depth() == 1; }
    void block(ImportCall& call, std::uint32_t key) const {
        call.block(key, config.threads->now_ns() + static_cast<std::uint64_t>(
                                                         std::chrono::duration_cast<std::chrono::nanoseconds>(retry_interval).count()));
    }
    static std::uint32_t key_of(std::uint32_t fd) { return 0x5e000000U | fd; }
    // Waits on the host for `events` on `fd` until `deadline` (nullopt: no limit). False on timeout.
    static bool host_wait(int fd, short events, std::optional<Clock::time_point> deadline) {
        while (true) {
            int ms = 100;
            if (deadline) {
                const auto left = std::chrono::duration_cast<std::chrono::milliseconds>(*deadline - Clock::now()).count();
                if (left <= 0) return false;
                ms = static_cast<int>(std::min<long long>(left, ms));
            }
            pollfd p{fd, events, 0};
            if (::poll(&p, 1, ms) > 0) return true;
        }
    }
    std::shared_ptr<Lookup> lookup(const std::string& name) {
        std::lock_guard lock(lookups_mutex);
        auto& entry = lookups[name];
        // A finished lookup is reused for a minute; a failed one for five seconds.
        if (entry && entry->done.load()) {
            const auto age = Clock::now() - entry->finished;
            if (age > (entry->addresses.empty() ? std::chrono::seconds(5) : std::chrono::seconds(60))) entry.reset();
        }
        if (!entry) {
            entry = std::make_shared<Lookup>();
            trace("lookup " + name);
            std::thread([entry, name] {
                addrinfo hints{};
                hints.ai_family = AF_INET;
                hints.ai_socktype = SOCK_STREAM;
                addrinfo* found = nullptr;
                const int status = ::getaddrinfo(name.c_str(), nullptr, &hints, &found);
                if (status != 0) entry->error = gai_strerror(status);
                for (auto* a = found; a != nullptr && entry->addresses.size() < 8; a = a->ai_next) {
                    if (a->ai_family != AF_INET) continue;
                    const auto value = reinterpret_cast<const sockaddr_in*>(a->ai_addr)->sin_addr.s_addr;
                    if (std::find(entry->addresses.begin(), entry->addresses.end(), value) == entry->addresses.end())
                        entry->addresses.push_back(value);
                }
                if (found) ::freeaddrinfo(found);
                if (entry->addresses.empty() && entry->error.empty()) entry->error = "no IPv4 address";
                entry->finished = Clock::now();
                entry->done.store(true);
            }).detach();
        }
        return entry;
    }
    // Replaces the host socket of `s` by one of `family`, keeping the options the guest set. False on failure.
    bool recreate(Socket& s, int family) {
        const int fd = ::socket(family, s.type, 0);
        if (fd < 0) return false;
        configure(fd);
        const auto apply = [fd](int level, int name, const std::optional<int>& value) {
            if (value) ::setsockopt(fd, level, name, &*value, sizeof(int));
        };
        apply(SOL_SOCKET, SO_RCVBUF, s.receive_buffer);
        apply(SOL_SOCKET, SO_SNDBUF, s.send_buffer);
        apply(SOL_SOCKET, SO_KEEPALIVE, s.keep_alive);
        apply(IPPROTO_TCP, TCP_NODELAY, s.no_delay);
        ::close(s.host);
        s.host = fd;
        return true;
    }
};

GuestSockets::GuestSockets(Config config) : state_(std::make_shared<State>()) { state_->config = std::move(config); }
GuestSockets::~GuestSockets() {
    for (auto& s : state_->sockets) if (s.host >= 0) ::close(s.host);
}
std::size_t GuestSockets::open_sockets() const noexcept {
    std::size_t n = 0;
    for (const auto& s : state_->sockets) n += s.host >= 0;
    return n;
}

void GuestSockets::install(ImportDispatcher& dispatcher) {
    auto state = state_;
    if (const auto* close = dispatcher.service("close")) state->previous_close = *close;

    // int socket(int domain, int type, int protocol)
    dispatcher.rebind("socket", [state](ImportCall& call) -> std::optional<std::string> {
        const std::uint32_t domain = call.arg(0), type = call.arg(1) & linux_sock_type_mask;
        if (domain != linux_af_inet) {
            state->trace("socket domain " + std::to_string(domain) + " unsupported");
            return state->fail(call, e_afnosupport, "socket");
        }
        if (type != linux_sock_stream && type != linux_sock_dgram) return state->fail(call, e_inval, "socket");
        State::Socket s;
        s.type = type == linux_sock_stream ? SOCK_STREAM : SOCK_DGRAM;
        s.guest_nonblocking = (call.arg(1) & linux_sock_nonblock) != 0;
        s.host = ::socket(AF_INET, s.type, 0);
        if (s.host < 0) return state->fail(call, guest_errno(errno), "socket");
        configure(s.host);
        std::size_t slot = 0;
        while (slot < state->sockets.size() && state->sockets[slot].host >= 0) ++slot;
        if (slot >= max_sockets) { ::close(s.host); return state->fail(call, e_mfile, "socket"); }
        if (slot == state->sockets.size()) state->sockets.emplace_back();
        state->sockets[slot] = std::move(s);
        call.set_result(first_descriptor + static_cast<std::uint32_t>(slot));
        return std::nullopt;
    });

    // int close(int fd): sockets here, every other descriptor where it was before.
    dispatcher.rebind("close", [state](ImportCall& call) -> std::optional<std::string> {
        const std::uint32_t fd = call.arg(0);
        if (fd >= first_descriptor && fd - first_descriptor < state->sockets.size()) {
            auto* s = state->socket(fd);
            if (!s) return state->fail(call, e_badf, "close");
            if (!s->peer.empty())
                state->trace("close " + s->peer + " sent=" + std::to_string(s->sent) + " received=" + std::to_string(s->received));
            ::close(s->host);
            *s = State::Socket{};
            call.set_result(0);
            return std::nullopt;
        }
        if (state->previous_close) return state->previous_close(call);
        return state->fail(call, e_badf, "close");
    });

    // int connect(int fd, const struct sockaddr* address, socklen_t length): IPv4 only.
    dispatcher.rebind("connect", [state](ImportCall& call) -> std::optional<std::string> {
        const std::uint32_t fd = call.arg(0);
        auto* s = state->socket(fd);
        if (!s) return state->fail(call, e_badf, "connect");
        if (!s->connect_deadline) {
            if (s->connected) return state->fail(call, e_isconn, "connect");
            std::array<std::byte, 8> raw{};
            if (call.arg(2) < 8 || !call.runtime().memory.read(call.arg(1), raw)) return state->fail(call, 14, "connect");
            const auto family = std::to_integer<std::uint32_t>(raw[0]) | std::to_integer<std::uint32_t>(raw[1]) << 8;
            if (family != linux_af_inet) return state->fail(call, e_afnosupport, "connect");
            const std::uint16_t port = static_cast<std::uint16_t>(std::to_integer<unsigned>(raw[2]) << 8 | std::to_integer<unsigned>(raw[3]));
            std::uint32_t address = 0;
            std::memcpy(&address, raw.data() + 4, 4);
            s->peer = ipv4_text(address) + ":" + std::to_string(port);
            // Resolve the IPv4 literal on the host: on an IPv6-only network (NAT64) the system answers with the
            // synthesized IPv6 address, which needs a socket of that family.
            sockaddr_storage target{};
            socklen_t target_length = 0;
            addrinfo hints{};
            hints.ai_family = AF_UNSPEC;
            hints.ai_socktype = s->type;
#ifdef AI_DEFAULT
            hints.ai_flags = AI_DEFAULT;
#endif
            addrinfo* found = nullptr;
            if (::getaddrinfo(ipv4_text(address).c_str(), std::to_string(port).c_str(), &hints, &found) == 0 && found) {
                std::memcpy(&target, found->ai_addr, found->ai_addrlen);
                target_length = found->ai_addrlen;
                ::freeaddrinfo(found);
            } else {
                auto* v4 = reinterpret_cast<sockaddr_in*>(&target);
                v4->sin_family = AF_INET;
                v4->sin_port = htons(port);
                v4->sin_addr.s_addr = address;
                target_length = sizeof(sockaddr_in);
            }
            if (target.ss_family != AF_INET && !state->recreate(*s, target.ss_family))
                return state->fail(call, guest_errno(errno), "connect");
            if (::connect(s->host, reinterpret_cast<const sockaddr*>(&target), target_length) == 0) {
                s->connected = true;
                state->trace("connect " + s->peer + " ok");
                call.set_result(0);
                return std::nullopt;
            }
            if (errno != EINPROGRESS) {
                const int error = errno;
                state->trace("connect " + s->peer + " failed: " + std::strerror(error));
                return state->fail(call, guest_errno(error), "connect");
            }
            if (s->guest_nonblocking) return state->fail(call, e_inprogress, "connect");
            const auto timeout = s->send_timeout_ms != 0 ? std::chrono::milliseconds(s->send_timeout_ms) : state->config.connect_timeout;
            s->connect_deadline = Clock::now() + std::min(timeout, state->config.connect_timeout);
        }
        while (true) {
            pollfd p{s->host, POLLOUT, 0};
            if (::poll(&p, 1, 0) > 0) {
                int error = 0;
                socklen_t length = sizeof error;
                ::getsockopt(s->host, SOL_SOCKET, SO_ERROR, &error, &length);
                s->connect_deadline.reset();
                if (error != 0) {
                    state->trace("connect " + s->peer + " failed: " + std::strerror(error));
                    return state->fail(call, guest_errno(error), "connect");
                }
                s->connected = true;
                state->trace("connect " + s->peer + " ok");
                call.set_result(0);
                return std::nullopt;
            }
            if (Clock::now() >= *s->connect_deadline) {
                s->connect_deadline.reset();
                state->trace("connect " + s->peer + " timed out");
                return state->fail(call, e_timedout, "connect");
            }
            if (state->can_block(call)) { state->block(call, State::key_of(fd)); return std::nullopt; }
            State::host_wait(s->host, POLLOUT, s->connect_deadline);
        }
    });

    // ssize_t send(int fd, const void* data, size_t length, int flags)
    dispatcher.rebind("send", [state](ImportCall& call) -> std::optional<std::string> {
        const std::uint32_t fd = call.arg(0);
        auto* s = state->socket(fd);
        if (!s) return state->fail(call, e_badf, "send");
        const std::size_t length = std::min<std::size_t>(call.arg(2), max_io);
        std::vector<std::byte> data(length);
        if (length != 0 && !call.runtime().memory.read(call.arg(1), data)) return state->fail(call, 14, "send");
        const bool dont_wait = s->guest_nonblocking || (call.arg(3) & linux_msg_dontwait) != 0;
        while (true) {
            const auto n = ::send(s->host, data.data(), data.size(), send_flags);
            if (n >= 0) {
                s->send_deadline.reset();
                s->sent += static_cast<std::uint64_t>(n);
                call.set_result(static_cast<std::uint32_t>(n));
                return std::nullopt;
            }
            if (errno != EAGAIN && errno != EWOULDBLOCK) {
                const int error = errno;
                s->send_deadline.reset();
                state->trace("send " + s->peer + " failed: " + std::strerror(error));
                return state->fail(call, guest_errno(error), "send");
            }
            if (dont_wait) return state->fail(call, e_again, "send");
            if (!s->send_deadline && s->send_timeout_ms != 0)
                s->send_deadline = Clock::now() + std::chrono::milliseconds(s->send_timeout_ms);
            if (s->send_deadline && Clock::now() >= *s->send_deadline) {
                s->send_deadline.reset();
                return state->fail(call, e_again, "send");
            }
            if (state->can_block(call)) {
                if (!s->send_deadline) s->send_deadline = Clock::time_point::max();  // marks the call as waiting
                state->block(call, State::key_of(fd));
                return std::nullopt;
            }
            if (!State::host_wait(s->host, POLLOUT, s->send_deadline == Clock::time_point::max() ? std::nullopt : s->send_deadline))
                return state->fail(call, e_again, "send");
        }
    });

    // ssize_t recv(int fd, void* buffer, size_t length, int flags)
    dispatcher.rebind("recv", [state](ImportCall& call) -> std::optional<std::string> {
        const std::uint32_t fd = call.arg(0);
        auto* s = state->socket(fd);
        if (!s) return state->fail(call, e_badf, "recv");
        const std::size_t length = std::min<std::size_t>(call.arg(2), max_io);
        int flags = 0;
        if (call.arg(3) & linux_msg_peek) flags |= MSG_PEEK;
        const bool dont_wait = s->guest_nonblocking || (call.arg(3) & linux_msg_dontwait) != 0;
        std::vector<std::byte> data(length);
        while (true) {
            const auto n = ::recv(s->host, data.data(), data.size(), flags);
            if (n >= 0) {
                s->receive_deadline.reset();
                if (n > 0 && !call.runtime().memory.write(call.arg(1), std::span<const std::byte>(data.data(), static_cast<std::size_t>(n))))
                    return state->fail(call, 14, "recv");
                if ((flags & MSG_PEEK) == 0) s->received += static_cast<std::uint64_t>(n);
                call.set_result(static_cast<std::uint32_t>(n));
                return std::nullopt;
            }
            if (errno != EAGAIN && errno != EWOULDBLOCK) {
                const int error = errno;
                s->receive_deadline.reset();
                state->trace("recv " + s->peer + " failed: " + std::strerror(error));
                return state->fail(call, guest_errno(error), "recv");
            }
            if (dont_wait) return state->fail(call, e_again, "recv");
            if (!s->receive_deadline)
                s->receive_deadline = s->receive_timeout_ms != 0 ? Clock::now() + std::chrono::milliseconds(s->receive_timeout_ms)
                                                                 : Clock::time_point::max();
            if (Clock::now() >= *s->receive_deadline) {
                s->receive_deadline.reset();
                state->trace("recv " + s->peer + " timed out");
                return state->fail(call, e_again, "recv");
            }
            if (state->can_block(call)) { state->block(call, State::key_of(fd)); return std::nullopt; }
            if (!State::host_wait(s->host, POLLIN, s->receive_deadline == Clock::time_point::max() ? std::nullopt : s->receive_deadline))
                return state->fail(call, e_again, "recv");
        }
    });

    // int setsockopt(int fd, int level, int name, const void* value, socklen_t length)
    dispatcher.rebind("setsockopt", [state](ImportCall& call) -> std::optional<std::string> {
        const std::uint32_t fd = call.arg(0), level = call.arg(1), name = call.arg(2);
        std::uint32_t length = 0;
        if (!call.read_arg(4, length)) return "setsockopt length is unreadable";
        auto* s = state->socket(fd);
        if (!s) return state->fail(call, e_badf, "setsockopt");
        std::array<std::byte, 8> raw{};
        if (length < 4 || !call.runtime().memory.read(call.arg(3), std::span<std::byte>(raw.data(), std::min<std::uint32_t>(length, 8))))
            return state->fail(call, e_inval, "setsockopt");
        const int value = static_cast<int>(le32(raw.data()));
        const auto set = [&](int host_level, int host_name, std::optional<int>& keep) -> std::optional<std::string> {
            keep = value;
            if (::setsockopt(s->host, host_level, host_name, &value, sizeof value) != 0)
                return state->fail(call, guest_errno(errno), "setsockopt");
            call.set_result(0);
            return std::nullopt;
        };
        if (level == linux_sol_socket && (name == linux_so_rcvtimeo || name == linux_so_sndtimeo)) {
            if (length < 8) return state->fail(call, e_inval, "setsockopt");
            const std::uint64_t ms = std::uint64_t{le32(raw.data())} * 1000 + le32(raw.data() + 4) / 1000;
            (name == linux_so_rcvtimeo ? s->receive_timeout_ms : s->send_timeout_ms) = static_cast<std::uint32_t>(std::min<std::uint64_t>(ms, 0xffffffffU));
            call.set_result(0);
            return std::nullopt;
        }
        if (level == linux_sol_socket && name == linux_so_rcvbuf) return set(SOL_SOCKET, SO_RCVBUF, s->receive_buffer);
        if (level == linux_sol_socket && name == linux_so_sndbuf) return set(SOL_SOCKET, SO_SNDBUF, s->send_buffer);
        if (level == linux_sol_socket && name == linux_so_keepalive) return set(SOL_SOCKET, SO_KEEPALIVE, s->keep_alive);
        if (level == linux_ipproto_tcp && name == linux_tcp_nodelay) return set(IPPROTO_TCP, TCP_NODELAY, s->no_delay);
        if (level == linux_sol_socket && (name == linux_so_reuseaddr || name == linux_so_linger)) { call.set_result(0); return std::nullopt; }
        state->trace("setsockopt level=" + std::to_string(level) + " name=" + std::to_string(name) + " ignored");
        call.set_result(0);
        return std::nullopt;
    });

    // int getsockopt(int fd, int level, int name, void* value, socklen_t* length)
    dispatcher.rebind("getsockopt", [state](ImportCall& call) -> std::optional<std::string> {
        const std::uint32_t fd = call.arg(0), level = call.arg(1), name = call.arg(2);
        std::uint32_t length_address = 0;
        if (!call.read_arg(4, length_address)) return "getsockopt length is unreadable";
        auto* s = state->socket(fd);
        if (!s) return state->fail(call, e_badf, "getsockopt");
        std::array<std::byte, 8> out{};
        std::uint32_t size = 4;
        const auto host_int = [&](int host_level, int host_name) {
            int value = 0;
            socklen_t length = sizeof value;
            ::getsockopt(s->host, host_level, host_name, &value, &length);
            return value;
        };
        if (level == linux_sol_socket && name == linux_so_error) {
            const int error = host_int(SOL_SOCKET, SO_ERROR);  // reading clears it
            put32(out.data(), error != 0 ? guest_errno(error) : 0);
        }
        else if (level == linux_sol_socket && name == linux_so_rcvbuf) put32(out.data(), static_cast<std::uint32_t>(host_int(SOL_SOCKET, SO_RCVBUF)));
        else if (level == linux_sol_socket && name == linux_so_sndbuf) put32(out.data(), static_cast<std::uint32_t>(host_int(SOL_SOCKET, SO_SNDBUF)));
        else if (level == linux_sol_socket && (name == linux_so_rcvtimeo || name == linux_so_sndtimeo)) {
            const std::uint32_t ms = name == linux_so_rcvtimeo ? s->receive_timeout_ms : s->send_timeout_ms;
            put32(out.data(), ms / 1000);
            put32(out.data() + 4, ms % 1000 * 1000);
            size = 8;
        } else if (level == linux_sol_socket && name == linux_so_keepalive) put32(out.data(), host_int(SOL_SOCKET, SO_KEEPALIVE) != 0);
        else if (level == linux_ipproto_tcp && name == linux_tcp_nodelay) put32(out.data(), host_int(IPPROTO_TCP, TCP_NODELAY) != 0);
        else state->trace("getsockopt level=" + std::to_string(level) + " name=" + std::to_string(name) + " answered 0");
        std::array<std::byte, 4> length_word{};
        if (!call.runtime().memory.read(length_address, length_word)) return state->fail(call, 14, "getsockopt");
        const std::uint32_t room = le32(length_word.data());
        size = std::min(size, room);
        if (size != 0 && !call.runtime().memory.write(call.arg(3), std::span<const std::byte>(out.data(), size))) return state->fail(call, 14, "getsockopt");
        put32(length_word.data(), size);
        if (!call.runtime().memory.write(length_address, length_word)) return state->fail(call, 14, "getsockopt");
        call.set_result(0);
        return std::nullopt;
    });

    // struct hostent* gethostbyname(const char* name): IPv4 addresses in one static result (as bionic's).
    dispatcher.rebind("gethostbyname", [state](ImportCall& call) -> std::optional<std::string> {
        std::string name;
        if (call.arg(0) == 0 || !read_guest_string(call.runtime().memory, call.arg(0), 1024, name)) { call.set_result(0); return std::nullopt; }
        std::vector<std::uint32_t> addresses;
        if (in_addr literal{}; ::inet_pton(AF_INET, name.c_str(), &literal) == 1) {
            addresses.push_back(literal.s_addr);
        } else {
            auto lookup = state->lookup(name);
            while (!lookup->done.load()) {
                if (Clock::now() - lookup->started > state->config.lookup_timeout) {
                    state->trace("lookup " + name + " timed out");
                    call.set_result(0);
                    return std::nullopt;
                }
                if (state->can_block(call)) { state->block(call, 0x5d000000U); return std::nullopt; }
                std::this_thread::sleep_for(retry_interval);
            }
            if (lookup->addresses.empty()) {
                state->trace("lookup " + name + " failed: " + lookup->error);
                call.set_result(0);
                return std::nullopt;
            }
            addresses = lookup->addresses;
        }
        // Layout in the data page: hostent at 0 {h_name, h_aliases, h_addrtype, h_length, h_addr_list},
        // the empty alias list at 0x20, the address list at 0x30, the addresses at 0x60, the name at 0x100.
        const std::uint32_t page = state->config.data_page;
        if (call.runtime().memory.translate(page, MemoryAccess::write) == nullptr)
            call.runtime().memory.map(page, Memory::page_size, Permissions::read_write);
        std::array<std::byte, Memory::page_size> block{};
        if (name.size() > 255) name.resize(255);
        std::memcpy(block.data() + 0x100, name.data(), name.size());
        put32(block.data() + 0, page + 0x100);
        put32(block.data() + 4, page + 0x20);
        put32(block.data() + 8, linux_af_inet);
        put32(block.data() + 12, 4);
        put32(block.data() + 16, page + 0x30);
        for (std::size_t i = 0; i < addresses.size() && i < 8; ++i) {
            put32(block.data() + 0x30 + 4 * i, page + 0x60 + 4 * static_cast<std::uint32_t>(i));
            std::memcpy(block.data() + 0x60 + 4 * i, &addresses[i], 4);
        }
        if (!call.runtime().memory.write(page, std::span<const std::byte>(block.data(), 0x200))) return "gethostbyname could not write its result";
        call.set_result(page);
        return std::nullopt;
    });
}
}
