// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include "atlan/imports.hpp"
#include <array>
#include <memory>
#include <string>
#include <vector>

namespace atlan {
// Minimal JavaVM and JNIEnv living in guest memory. Both objects and their
// function tables are read-only guest data; every table slot holds a distinct
// unmapped trap address, so calling a JNI function the host has not implemented
// stops execution naming that function (e.g. "JNIEnv::FindClass") instead of
// returning a made-up value. Services are bound one by one, only when observed.
class JavaBootstrap {
public:
    static constexpr std::uint32_t version_1_6 = 0x00010006;
    static constexpr std::size_t invoke_slots = 8, native_slots = 233;
    struct Config {
        std::uint32_t data_base{0xf1000000};  // one page of read-only tables
        std::uint32_t trap_base{0xf2000000};  // unmapped function trap range
    };
    // Maps the tables into runtime.memory and binds the implemented services.
    // Throws std::invalid_argument if the regions overlap existing mappings.
    JavaBootstrap(Runtime& runtime, ImportDispatcher& dispatcher, Config config);
    // A class registered through RegisterNatives; guest addresses only.
    struct NativeMethod {
        std::string class_name, name, signature;
        std::uint32_t address{};  // guest function, Thumb bit preserved
    };
    // A jmethodID / jfieldID obtained from Get[Static]MethodID / Get[Static]FieldID.
    struct Member {
        enum class Kind : std::uint8_t { method, static_method, field, static_field };
        Kind kind{};
        std::string class_name, name, signature;
    };
    [[nodiscard]] const std::vector<Member>& members() const noexcept { return state_->members; }
    // Opaque jclass handles are host-table indexes encoded as unmapped guest
    // addresses; they are never dereferenceable and never host pointers.
    [[nodiscard]] const std::vector<std::string>& classes() const noexcept { return state_->classes; }
    [[nodiscard]] const std::vector<NativeMethod>& native_methods() const noexcept { return state_->natives; }
    // Objects created through NewObject*; only the minimal java/lang/String
    // (empty, via the ()V constructor) exists so far. Handles are opaque.
    [[nodiscard]] const std::vector<std::string>& objects() const noexcept { return state_->objects; }
    // Live global references (NewGlobalRef minus DeleteGlobalRef).
    [[nodiscard]] std::size_t live_global_refs() const noexcept {
        std::size_t n = 0;
        for (auto g : state_->globals) n += g != 0;
        return n;
    }
    [[nodiscard]] std::uint32_t vm() const noexcept { return vm_; }
    [[nodiscard]] std::uint32_t env() const noexcept { return env_; }
    // Names of table slots, e.g. "JavaVM::GetEnv"; empty for reserved slots.
    [[nodiscard]] static const std::string& invoke_name(std::size_t slot);
    [[nodiscard]] static const std::string& native_name(std::size_t slot);
private:
    struct State {
        std::vector<std::string> classes;
        std::vector<NativeMethod> natives;
        std::vector<Member> members;
        std::vector<std::string> objects;  // object class names; index = handle slot
        std::vector<std::uint32_t> globals;  // referent handle; 0 once deleted
    };
    std::shared_ptr<State> state_ = std::make_shared<State>();
    std::uint32_t vm_{}, env_{};
};
}
