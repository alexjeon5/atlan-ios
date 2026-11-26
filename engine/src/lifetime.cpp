// SPDX-License-Identifier: GPL-2.0-or-later
#include "atlan/lifetime.hpp"

namespace atlan {
std::optional<std::string> ObjectLifetime::add(Entry entry) {
    if (entry.function == 0) return "atexit registration with null destructor";
    if (entries_.size() >= max_entries) return "atexit registry is full";
    entries_.push_back(entry);
    return std::nullopt;
}
std::optional<std::string> ObjectLifetime::finalize(ImportDispatcher& dispatcher,
                                                    Runtime& runtime, std::uint32_t dso) {
    for (std::size_t i = entries_.size(); i-- > 0;) {
        if (i >= entries_.size() || (dso != 0 && entries_[i].dso != dso)) continue;
        const Entry entry = entries_[i];
        entries_.erase(entries_.begin() + static_cast<std::ptrdiff_t>(i));
        const std::array<std::uint32_t, 1> args{entry.object};
        const auto outcome = dispatcher.call_back(runtime, entry.function, args);
        if (outcome.stop != Stop::returned)
            return "guest destructor did not return (stop=" + std::to_string(static_cast<int>(outcome.stop)) +
                   (outcome.import.empty() ? "" : ", import=" + outcome.import) + ")";
    }
    return std::nullopt;
}
void ObjectLifetime::install(ImportDispatcher& dispatcher) {
    // int __aeabi_atexit(void* object, void (*dtor)(void*), void* dso)
    dispatcher.bind("__aeabi_atexit", [this](ImportCall& call) {
        auto error = add({call.arg(1), call.arg(0), call.arg(2)});
        if (!error) call.set_result(0);
        return error;
    });
    // int __cxa_atexit(void (*dtor)(void*), void* object, void* dso)
    dispatcher.bind("__cxa_atexit", [this](ImportCall& call) {
        auto error = add({call.arg(0), call.arg(1), call.arg(2)});
        if (!error) call.set_result(0);
        return error;
    });
    // void __cxa_finalize(void* dso)
    dispatcher.bind("__cxa_finalize", [this](ImportCall& call) {
        return finalize(call.dispatcher(), call.runtime(), call.arg(0));
    });
}
}
