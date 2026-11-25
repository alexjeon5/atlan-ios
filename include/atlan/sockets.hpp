// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include "atlan/imports.hpp"
#include <chrono>
#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <string>

namespace atlan {
class GuestThreads;

// BSD sockets for the guest, backed by host sockets: socket, connect, send, recv, setsockopt, getsockopt and
// gethostbyname (IPv4, as bionic's), plus close for socket descriptors (other descriptors go to the close service
// bound before install()). Constants and errno values are ARM Linux ones and are translated.
//
// Host sockets are always non-blocking. A guest call that would block (connect in progress, recv/send without data
// or room, a name lookup still running) blocks only the calling guest thread: it is retried every few milliseconds
// through the guest thread scheduler until it completes or its timeout (SO_RCVTIMEO/SO_SNDTIMEO, or a default for
// connect) passes. Where blocking is impossible (a nested host callback) the call waits on the host instead.
// Name lookups run on a host thread and are cached for a minute.
class GuestSockets {
public:
    struct Config {
        std::uint32_t errno_address{};
        std::uint32_t data_page{0xf7000000};  // one read-write page for gethostbyname's result
        GuestThreads* threads{};              // nullptr: every wait happens on the host
        std::function<void(const std::string&)> trace;  // one line per connection event (no payload)
        std::chrono::milliseconds connect_timeout{20000};
        std::chrono::milliseconds lookup_timeout{15000};
    };
    static constexpr std::uint32_t first_descriptor = 0x400;  // guest socket descriptors: first_descriptor + slot
    explicit GuestSockets(Config config);
    ~GuestSockets();
    GuestSockets(const GuestSockets&) = delete;
    GuestSockets& operator=(const GuestSockets&) = delete;
    // Binds the services (the data page is mapped on first use). Bind file descriptor services (close) first.
    void install(ImportDispatcher& dispatcher);
    [[nodiscard]] std::size_t open_sockets() const noexcept;
private:
    struct State;
    std::shared_ptr<State> state_;
};
}
