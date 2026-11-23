// SPDX-License-Identifier: GPL-2.0-or-later
#include "atlan/threads.hpp"
#include <array>

namespace atlan {
namespace {
constexpr std::uint32_t eperm = 1, ebusy = 16, edeadlk = 35, einval = 22;
bool write_word(Memory& memory, std::uint32_t at, std::uint32_t value) {
    std::array<std::byte, 4> bytes{};
    for (unsigned i = 0; i < 4; ++i) bytes[i] = std::byte((value >> (8 * i)) & 255);
    return memory.write(at, bytes);
}
bool read_word(Memory& memory, std::uint32_t at, std::uint32_t& value) {
    std::array<std::byte, 4> bytes{};
    if (!memory.read(at, bytes)) return false;
    value = 0;
    for (unsigned i = 0; i < 4; ++i) value |= std::to_integer<std::uint32_t>(bytes[i]) << (8 * i);
    return true;
}
}
void MutexTable::install(ImportDispatcher& dispatcher) {
    // int pthread_mutexattr_init(pthread_mutexattr_t*)
    dispatcher.bind("pthread_mutexattr_init", [](ImportCall& call) -> std::optional<std::string> {
        if (!write_word(call.runtime().memory, call.arg(0), 0)) return "pthread_mutexattr_init guest memory fault";
        call.set_result(0);
        return std::nullopt;
    });
    // int pthread_mutexattr_settype(pthread_mutexattr_t*, int type)
    dispatcher.bind("pthread_mutexattr_settype", [](ImportCall& call) -> std::optional<std::string> {
        if (call.arg(1) > 2) { call.set_result(einval); return std::nullopt; }
        if (!write_word(call.runtime().memory, call.arg(0), call.arg(1))) return "pthread_mutexattr_settype guest memory fault";
        call.set_result(0);
        return std::nullopt;
    });
    // int pthread_mutexattr_destroy(pthread_mutexattr_t*)
    dispatcher.bind("pthread_mutexattr_destroy", [](ImportCall& call) -> std::optional<std::string> {
        call.set_result(0);
        return std::nullopt;
    });
    // int pthread_mutex_init(pthread_mutex_t*, const pthread_mutexattr_t*)
    dispatcher.bind("pthread_mutex_init", [this](ImportCall& call) -> std::optional<std::string> {
        std::uint32_t type = 0;
        if (call.arg(1) != 0 && !read_word(call.runtime().memory, call.arg(1), type))
            return "pthread_mutex_init could not read the attribute";
        if (type > 2) { call.set_result(einval); return std::nullopt; }
        if (call.arg(0) == 0) return "pthread_mutex_init on a null mutex";
        mutexes_[call.arg(0)] = {type, 0};
        call.set_result(0);
        return std::nullopt;
    });
    // int pthread_mutex_destroy(pthread_mutex_t*)
    dispatcher.bind("pthread_mutex_destroy", [this](ImportCall& call) -> std::optional<std::string> {
        const auto found = mutexes_.find(call.arg(0));
        if (found != mutexes_.end() && found->second.count != 0) { call.set_result(ebusy); return std::nullopt; }
        mutexes_.erase(call.arg(0));
        call.set_result(0);
        return std::nullopt;
    });
    // int pthread_mutex_lock(pthread_mutex_t*)
    dispatcher.bind("pthread_mutex_lock", [this](ImportCall& call) -> std::optional<std::string> {
        const auto address = call.arg(0);
        if (address == 0) return "pthread_mutex_lock on a null mutex";
        auto& state = mutexes_[address];
        if (state.count == 0 || state.type == 1) {
            if (state.count == 0xffffffffU) return "recursive mutex lock count overflow";
            ++state.count;
            call.set_result(0);
        } else if (state.type == 2) {
            call.set_result(edeadlk);
        } else {
            return "pthread_mutex_lock would deadlock: mutex is already held by the only guest thread";
        }
        return std::nullopt;
    });
    // int pthread_mutex_unlock(pthread_mutex_t*)
    dispatcher.bind("pthread_mutex_unlock", [this](ImportCall& call) -> std::optional<std::string> {
        const auto found = mutexes_.find(call.arg(0));
        if (found == mutexes_.end() || found->second.count == 0) {
            if (found != mutexes_.end() && found->second.type != 0) { call.set_result(eperm); return std::nullopt; }
            return "pthread_mutex_unlock of a mutex that is not held";
        }
        --found->second.count;
        call.set_result(0);
        return std::nullopt;
    });
}
}
