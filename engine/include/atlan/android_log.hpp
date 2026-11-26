// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include "atlan/imports.hpp"
#include <functional>
#include <string>

namespace atlan {
struct LogEntry {
    std::uint32_t priority{};  // android_LogPriority as passed by the guest
    std::string tag;
    std::string message;
};
// Host side of __android_log_write / __android_log_print / __android_log_vprint.
// Entries go only to the sink; nothing is forwarded to a host syscall or file.
// The sink must outlive every dispatcher that has the service installed.
class AndroidLog {
public:
    using Sink = std::function<void(const LogEntry&)>;
    explicit AndroidLog(Sink sink) : sink_(std::move(sink)) {}
    void install(ImportDispatcher& dispatcher);
private:
    Sink sink_;
};
}
