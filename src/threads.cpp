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
    // int pthread_once(pthread_once_t*, void (*init)(void)): 0 = not run, 1 = running, 2 = done.
    dispatcher.bind("pthread_once", [](ImportCall& call) -> std::optional<std::string> {
        const auto control = call.arg(0), routine = call.arg(1);
        auto& memory = call.runtime().memory;
        std::uint32_t state = 0;
        if (!read_word(memory, control, state)) return "pthread_once could not read the control word";
        if (state == 2) { call.set_result(0); return std::nullopt; }
        if (state == 1) return "pthread_once re-entered by its own init routine (deadlock)";
        if (routine == 0) return "pthread_once with a null init routine";
        if (!write_word(memory, control, 1)) return "pthread_once guest memory fault";
        const auto outcome = call.dispatcher().call_back(call.runtime(), routine, {});
        if (outcome.stop != Stop::returned)
            return "pthread_once init routine did not return" +
                   (outcome.import.empty() ? std::string() : " (import " + outcome.import + ")");
        if (!write_word(memory, control, 2)) return "pthread_once guest memory fault";
        call.set_result(0);
        return std::nullopt;
    });
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

void SemaphoreTable::install(ImportDispatcher& dispatcher) {
    constexpr std::uint32_t sem_value_max = 0x7fffffff;
    // int sem_init(sem_t*, int pshared, unsigned value)
    dispatcher.bind("sem_init", [this](ImportCall& call) -> std::optional<std::string> {
        if (call.arg(0) == 0) return "sem_init on a null semaphore";
        if (call.arg(1) != 0) return "sem_init with pshared != 0 is not implemented";
        if (call.arg(2) > sem_value_max) { call.set_result(0xffffffffU); return "sem_init value exceeds SEM_VALUE_MAX (errno EINVAL is not modelled)"; }
        counts_[call.arg(0)] = call.arg(2);
        call.set_result(0);
        return std::nullopt;
    });
    // int sem_destroy(sem_t*)
    dispatcher.bind("sem_destroy", [this](ImportCall& call) -> std::optional<std::string> {
        if (counts_.erase(call.arg(0)) == 0) return "sem_destroy of a semaphore that was not initialized";
        call.set_result(0);
        return std::nullopt;
    });
    // int sem_post(sem_t*)
    dispatcher.bind("sem_post", [this](ImportCall& call) -> std::optional<std::string> {
        const auto found = counts_.find(call.arg(0));
        if (found == counts_.end()) return "sem_post on a semaphore that was not initialized";
        if (found->second == sem_value_max) return "sem_post would overflow SEM_VALUE_MAX";
        ++found->second;
        call.set_result(0);
        return std::nullopt;
    });
    // int sem_wait(sem_t*)
    dispatcher.bind("sem_wait", [this](ImportCall& call) -> std::optional<std::string> {
        const auto found = counts_.find(call.arg(0));
        if (found == counts_.end()) return "sem_wait on a semaphore that was not initialized";
        if (found->second == 0) return "sem_wait would deadlock: count is zero and there is only one guest thread";
        --found->second;
        call.set_result(0);
        return std::nullopt;
    });
}

void ThreadKeys::install(ImportDispatcher& dispatcher) {
    // int pthread_key_create(pthread_key_t* key, void (*destructor)(void*))
    dispatcher.bind("pthread_key_create", [this](ImportCall& call) -> std::optional<std::string> {
        std::uint32_t index = 0;
        while (index < keys_.size() && keys_[index].live) ++index;
        if (index == max_keys) { call.set_result(11); return std::nullopt; }  // EAGAIN
        if (index == keys_.size()) keys_.emplace_back();
        if (!write_word(call.runtime().memory, call.arg(0), index)) return "pthread_key_create guest memory fault";
        keys_[index] = {true, 0, call.arg(1)};
        call.set_result(0);
        return std::nullopt;
    });
    // int pthread_key_delete(pthread_key_t)
    dispatcher.bind("pthread_key_delete", [this](ImportCall& call) -> std::optional<std::string> {
        if (call.arg(0) >= keys_.size() || !keys_[call.arg(0)].live) { call.set_result(einval); return std::nullopt; }
        keys_[call.arg(0)] = {};
        call.set_result(0);
        return std::nullopt;
    });
    // void* pthread_getspecific(pthread_key_t): NULL for an invalid key, as in POSIX.
    dispatcher.bind("pthread_getspecific", [this](ImportCall& call) -> std::optional<std::string> {
        call.set_result(call.arg(0) < keys_.size() && keys_[call.arg(0)].live ? keys_[call.arg(0)].value : 0);
        return std::nullopt;
    });
    // int pthread_setspecific(pthread_key_t, const void*)
    dispatcher.bind("pthread_setspecific", [this](ImportCall& call) -> std::optional<std::string> {
        if (call.arg(0) >= keys_.size() || !keys_[call.arg(0)].live) { call.set_result(einval); return std::nullopt; }
        keys_[call.arg(0)].value = call.arg(1);
        call.set_result(0);
        return std::nullopt;
    });
}
}
