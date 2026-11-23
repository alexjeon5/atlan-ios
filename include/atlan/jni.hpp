// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include "atlan/imports.hpp"
#include <array>
#include <functional>
#include <map>
#include <memory>
#include <optional>
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
        std::uint32_t scratch_base{0xf4000000};  // read-write guest memory for string copies
        std::uint32_t scratch_size{1U << 20};
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
    // Objects created through NewObject*: opaque host objects holding only a class
    // name and a field store (any class, via its ()V constructor). They have no
    // behaviour beyond host-registered methods. Handles are opaque.
    [[nodiscard]] const std::vector<std::string>& objects() const noexcept { return state_->objects; }
    // One Java-side method call made by the guest, for a host-registered method.
    // `args` are raw values in signature order: 32-bit types zero-extended, J/D as
    // 64-bit bits, F as float bits, references as opaque handles. The handler
    // stores the raw return value in `result` (references as handles).
    struct MethodCall {
        std::string class_name, name, signature;
        std::uint32_t receiver{};  // object handle, or class handle for static calls
        std::vector<std::uint64_t> args;
        std::uint64_t result{};
    };
    using MethodHandler = std::function<std::optional<std::string>(MethodCall&)>;
    // Registers the host implementation of one Java method. Only registered
    // methods can be called by the guest; any other call stops by name.
    // False if (class, name, signature) is already registered.
    bool register_method(std::string class_name, std::string name, std::string signature, MethodHandler handler);
    // Host-side handle minting, for calling registered natives from the embedder:
    // the class handle for a name (created if unseen), and a new opaque object of
    // that class. 0 means the reference tables are full.
    [[nodiscard]] std::uint32_t class_handle(const std::string& class_name);
    [[nodiscard]] std::uint32_t new_object(std::uint32_t class_handle);
    // A java/lang/String host object with content (UTF-8, no embedded NUL).
    // Returns 0 if the object table is full or the text is unusable.
    [[nodiscard]] std::uint32_t new_string(const std::string& utf8);
    // Content of a String object handle, if it is one.
    [[nodiscard]] std::optional<std::string> string_text(std::uint32_t handle) const;
    // byte[] host objects (the only array type modelled so far).
    [[nodiscard]] std::uint32_t new_byte_array(const std::vector<std::uint8_t>& bytes);
    [[nodiscard]] std::optional<std::vector<std::uint8_t>> byte_array(std::uint32_t handle) const;
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
        std::vector<std::string> texts;  // per object: String content ("" otherwise)
        std::vector<bool> is_string;
        std::vector<std::vector<std::uint8_t>> byte_arrays;  // handle slot -> contents
        std::map<std::uint32_t, std::uint32_t> array_copies;  // scratch address -> array handle
        std::map<std::uint32_t, std::uint32_t> copies;  // guest scratch copy address -> size
        std::uint32_t scratch_base{}, scratch_size{}, scratch_used{};
        std::vector<std::map<std::uint32_t, std::uint64_t>> fields;  // per object: field id -> raw value
        std::map<std::string, MethodHandler> methods;  // "class\x1fname\x1fsignature"
        std::vector<std::uint32_t> globals;  // referent handle; 0 once deleted
    };
    std::shared_ptr<State> state_ = std::make_shared<State>();
    std::uint32_t vm_{}, env_{};
};
}
