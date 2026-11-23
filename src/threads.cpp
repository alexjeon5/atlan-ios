// SPDX-License-Identifier: GPL-2.0-or-later
#include "atlan/threads.hpp"

namespace atlan {
void MutexTable::install(ImportDispatcher& dispatcher) {
    // int pthread_mutex_lock(pthread_mutex_t*)
    dispatcher.bind("pthread_mutex_lock", [this](ImportCall& call) -> std::optional<std::string> {
        const auto mutex = call.arg(0);
        if (mutex == 0) return "pthread_mutex_lock on a null mutex";
        if (!held_.insert(mutex).second) return "pthread_mutex_lock would deadlock: mutex is already held by the only guest thread";
        call.set_result(0);
        return std::nullopt;
    });
    // int pthread_mutex_unlock(pthread_mutex_t*)
    dispatcher.bind("pthread_mutex_unlock", [this](ImportCall& call) -> std::optional<std::string> {
        if (held_.erase(call.arg(0)) == 0) return "pthread_mutex_unlock of a mutex that is not held";
        call.set_result(0);
        return std::nullopt;
    });
}
}
