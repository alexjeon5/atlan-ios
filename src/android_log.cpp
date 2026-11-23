// SPDX-License-Identifier: GPL-2.0-or-later
#include "atlan/android_log.hpp"
#include "atlan/abi.hpp"

namespace atlan {
namespace {
constexpr std::size_t max_tag = 128, max_text = 4096;
}
void AndroidLog::install(ImportDispatcher& dispatcher) {
    // The guest result is the logged message length. Real Bionic returns the
    // write result; callers observed so far ignore it (assumed, not verified).
    const auto emit = [this](ImportCall& call, std::string message) {
        LogEntry entry;
        entry.priority = call.arg(0);
        if (call.arg(1) != 0 && !read_guest_string(call.runtime().memory, call.arg(1), max_tag, entry.tag))
            return std::optional<std::string>("android log tag is unreadable or too long");
        entry.message = std::move(message);
        call.set_result(static_cast<std::uint32_t>(entry.message.size()));
        sink_(entry);
        return std::optional<std::string>{};
    };
    // int __android_log_write(int prio, const char* tag, const char* text)
    dispatcher.bind("__android_log_write", [emit](ImportCall& call) -> std::optional<std::string> {
        std::string text;
        if (call.arg(2) == 0 || !read_guest_string(call.runtime().memory, call.arg(2), max_text, text))
            return "__android_log_write text is null, unreadable or too long";
        return emit(call, std::move(text));
    });
    // int __android_log_print(int prio, const char* tag, const char* fmt, ...)
    dispatcher.bind("__android_log_print", [emit](ImportCall& call) -> std::optional<std::string> {
        if (call.arg(2) == 0) return "__android_log_print format is null";
        auto args = VarArgs::from_call(call, 3);
        std::string text;
        if (auto error = format_guest_printf(call.runtime().memory, call.arg(2), args, text)) return error;
        return emit(call, std::move(text));
    });
    // int __android_log_vprint(int prio, const char* tag, const char* fmt, va_list ap)
    dispatcher.bind("__android_log_vprint", [emit](ImportCall& call) -> std::optional<std::string> {
        if (call.arg(2) == 0) return "__android_log_vprint format is null";
        auto args = VarArgs::from_va_list(call.runtime().memory, call.arg(3));
        std::string text;
        if (auto error = format_guest_printf(call.runtime().memory, call.arg(2), args, text)) return error;
        return emit(call, std::move(text));
    });
}
}
