// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include <atomic>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>

namespace atlan::shell {
enum class SessionState : std::uint8_t { idle, running, finished, failed };

struct SessionConfig {
    std::string library_path;                   // user-supplied ARM32 ELF32 shared object
    std::uint64_t initializer_budget{3000000};  // retired instructions per initializer
};

// Loads the guest image and runs its constructors on one dedicated thread.
// `log` is called from that thread; the embedder must hop to its UI thread.
// stop() is cooperative and takes effect between initializers.
class GuestSession {
public:
    using Log = std::function<void(const std::string&)>;
    GuestSession(SessionConfig config, Log log);
    ~GuestSession();
    GuestSession(const GuestSession&) = delete;
    GuestSession& operator=(const GuestSession&) = delete;
    bool start();  // false if already started or the thread cannot be created
    void stop() noexcept { stop_.store(true); }
    void join() noexcept;
    [[nodiscard]] SessionState state() const noexcept { return state_.load(); }
private:
    struct Thread;
    void run();
    SessionConfig config_;
    Log log_;
    std::atomic<bool> stop_{false};
    std::atomic<SessionState> state_{SessionState::idle};
    std::unique_ptr<Thread> thread_;
};
}
