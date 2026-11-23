// SPDX-License-Identifier: GPL-2.0-or-later
#include "atlan/jni.hpp"
#include "atlan/abi.hpp"
#include <stdexcept>

namespace atlan {
namespace {
constexpr std::uint32_t class_handle_base = 0xc0000000, member_handle_base = 0xd0000000, object_handle_base = 0xe0000000, global_handle_base = 0xe8000000;
std::vector<std::string> make_native_names() {
    std::vector<std::string> names(4);  // reserved0-3
    const auto add = [&](std::string name) { names.push_back("JNIEnv::" + std::move(name)); };
    for (const char* n : {"GetVersion", "DefineClass", "FindClass", "FromReflectedMethod", "FromReflectedField",
                          "ToReflectedMethod", "GetSuperclass", "IsAssignableFrom", "ToReflectedField", "Throw",
                          "ThrowNew", "ExceptionOccurred", "ExceptionDescribe", "ExceptionClear", "FatalError",
                          "PushLocalFrame", "PopLocalFrame", "NewGlobalRef", "DeleteGlobalRef", "DeleteLocalRef",
                          "IsSameObject", "NewLocalRef", "EnsureLocalCapacity", "AllocObject", "NewObject",
                          "NewObjectV", "NewObjectA", "GetObjectClass", "IsInstanceOf", "GetMethodID"})
        add(n);
    const char* const types[] = {"Object", "Boolean", "Byte", "Char", "Short", "Int", "Long", "Float", "Double", "Void"};
    for (const char* prefix : {"Call", "CallNonvirtual"})
        for (const char* type : types)
            for (const char* suffix : {"", "V", "A"})
                add(std::string(prefix) + type + "Method" + suffix);
    add("GetFieldID");
    for (const char* type : types) if (std::string_view(type) != "Void") add(std::string("Get") + type + "Field");
    for (const char* type : types) if (std::string_view(type) != "Void") add(std::string("Set") + type + "Field");
    add("GetStaticMethodID");
    for (const char* type : types)
        for (const char* suffix : {"", "V", "A"}) add(std::string("CallStatic") + type + "Method" + suffix);
    add("GetStaticFieldID");
    for (const char* type : types) if (std::string_view(type) != "Void") add(std::string("GetStatic") + type + "Field");
    for (const char* type : types) if (std::string_view(type) != "Void") add(std::string("SetStatic") + type + "Field");
    for (const char* n : {"NewString", "GetStringLength", "GetStringChars", "ReleaseStringChars", "NewStringUTF",
                          "GetStringUTFLength", "GetStringUTFChars", "ReleaseStringUTFChars", "GetArrayLength",
                          "NewObjectArray", "GetObjectArrayElement", "SetObjectArrayElement"})
        add(n);
    const char* const primitives[] = {"Boolean", "Byte", "Char", "Short", "Int", "Long", "Float", "Double"};
    for (const char* t : primitives) add(std::string("New") + t + "Array");
    for (const char* t : primitives) add(std::string("Get") + t + "ArrayElements");
    for (const char* t : primitives) add(std::string("Release") + t + "ArrayElements");
    for (const char* t : primitives) add(std::string("Get") + t + "ArrayRegion");
    for (const char* t : primitives) add(std::string("Set") + t + "ArrayRegion");
    for (const char* n : {"RegisterNatives", "UnregisterNatives", "MonitorEnter", "MonitorExit", "GetJavaVM",
                          "GetStringRegion", "GetStringUTFRegion", "GetPrimitiveArrayCritical",
                          "ReleasePrimitiveArrayCritical", "GetStringCritical", "ReleaseStringCritical",
                          "NewWeakGlobalRef", "DeleteWeakGlobalRef", "ExceptionCheck", "NewDirectByteBuffer",
                          "GetDirectBufferAddress", "GetDirectBufferCapacity", "GetObjectRefType"})
        add(n);
    return names;
}
const std::vector<std::string>& native_names() {
    static const std::vector<std::string> names = make_native_names();
    return names;
}
const std::vector<std::string>& invoke_names() {
    static const std::vector<std::string> names{"", "", "", "JavaVM::DestroyJavaVM", "JavaVM::AttachCurrentThread",
                                                "JavaVM::DetachCurrentThread", "JavaVM::GetEnv",
                                                "JavaVM::AttachCurrentThreadAsDaemon"};
    return names;
}
void put(std::vector<std::byte>& bytes, std::size_t at, std::uint32_t value) {
    for (unsigned i = 0; i < 4; ++i) bytes[at + i] = std::byte((value >> (8 * i)) & 255);
}
bool put_guest(Memory& memory, std::uint32_t at, std::uint32_t value) {
    std::array<std::byte, 4> bytes{};
    for (unsigned i = 0; i < 4; ++i) bytes[i] = std::byte((value >> (8 * i)) & 255);
    return memory.write(at, bytes);
}
}
const std::string& JavaBootstrap::invoke_name(std::size_t slot) { return invoke_names().at(slot); }
const std::string& JavaBootstrap::native_name(std::size_t slot) { return native_names().at(slot); }

JavaBootstrap::JavaBootstrap(Runtime& runtime, ImportDispatcher& dispatcher, Config config) {
    if (native_names().size() != native_slots) throw std::logic_error("JNI function table size mismatch");
    // Layout inside one read-only page: vm object, invoke table, env object, native table.
    constexpr std::uint32_t vm_off = 0x00, invoke_off = 0x10, env_off = 0x40, native_off = 0x100;
    static_assert(native_off + 4 * native_slots <= Memory::page_size);
    if (config.data_base % Memory::page_size != 0 || config.trap_base % 4 != 0 ||
        config.trap_base == 0 || runtime.memory.translate(config.trap_base, MemoryAccess::read) != nullptr)
        throw std::invalid_argument("invalid JNI bootstrap layout");
    vm_ = config.data_base + vm_off;
    env_ = config.data_base + env_off;
    const std::uint32_t invoke_traps = config.trap_base, native_traps = config.trap_base + 4 * invoke_slots;
    std::vector<std::byte> page(Memory::page_size);
    put(page, vm_off, config.data_base + invoke_off);
    put(page, env_off, config.data_base + native_off);
    for (std::size_t i = 0; i < invoke_slots; ++i) put(page, invoke_off + 4 * i, invoke_traps + 4 * static_cast<std::uint32_t>(i));
    for (std::size_t i = 0; i < native_slots; ++i) put(page, native_off + 4 * i, native_traps + 4 * static_cast<std::uint32_t>(i));
    runtime.memory.map(config.data_base, Memory::page_size, Permissions::read, page);
    for (std::size_t i = 0; i < invoke_slots; ++i)
        if (!invoke_names()[i].empty()) (void)dispatcher.bind_trap(invoke_traps + 4 * static_cast<std::uint32_t>(i), invoke_names()[i]);
    for (std::size_t i = 0; i < native_slots; ++i)
        if (!native_names()[i].empty()) (void)dispatcher.bind_trap(native_traps + 4 * static_cast<std::uint32_t>(i), native_names()[i]);

    constexpr std::size_t max_classes = 4096, max_natives = 4096, max_members = 16384, max_objects = 65536, max_globals = 65536, max_name = 256;
    const std::uint32_t env_check = env_;
    const auto state = state_;
    // jclass JNIEnv::FindClass(JNIEnv* env, const char* name): returns an opaque
    // reference for a class name. Existence of the class is not checked here (no
    // Java side yet); the name is recorded so later calls can be tied to it.
    dispatcher.bind("JNIEnv::FindClass", [state, env_check](ImportCall& call) -> std::optional<std::string> {
        if (call.arg(0) != env_check) return "JNIEnv::FindClass called with a foreign JNIEnv pointer";
        std::string name;
        if (call.arg(1) == 0 || !read_guest_string(call.runtime().memory, call.arg(1), max_name, name) || name.empty())
            return "JNIEnv::FindClass name is null, empty, unreadable or too long";
        for (std::size_t i = 0; i < state->classes.size(); ++i)
            if (state->classes[i] == name) { call.set_result(class_handle_base + 4 * static_cast<std::uint32_t>(i + 1)); return std::nullopt; }
        if (state->classes.size() >= max_classes) return "JNIEnv::FindClass reference table is full";
        state->classes.push_back(std::move(name));
        call.set_result(class_handle_base + 4 * static_cast<std::uint32_t>(state->classes.size()));
        return std::nullopt;
    });
    // jint JNIEnv::RegisterNatives(JNIEnv*, jclass, const JNINativeMethod*, jint count)
    // JNINativeMethod is {const char* name; const char* signature; void* fnPtr} (12 bytes).
    dispatcher.bind("JNIEnv::RegisterNatives", [state, env_check](ImportCall& call) -> std::optional<std::string> {
        if (call.arg(0) != env_check) return "JNIEnv::RegisterNatives called with a foreign JNIEnv pointer";
        const auto handle = call.arg(1), table = call.arg(2);
        const auto count = static_cast<std::int32_t>(call.arg(3));
        if (handle < class_handle_base + 4 || (handle - class_handle_base) % 4 != 0 ||
            (handle - class_handle_base) / 4 > state->classes.size())
            return "JNIEnv::RegisterNatives class reference is not valid";
        if (count < 0 || state->natives.size() + static_cast<std::size_t>(count) > max_natives)
            return "JNIEnv::RegisterNatives count is invalid or exceeds the registration bound";
        std::vector<JavaBootstrap::NativeMethod> staged;
        for (std::int32_t i = 0; i < count; ++i) {
            std::array<std::byte, 12> entry{};
            if (std::uint64_t{table} + 12ULL * static_cast<std::uint32_t>(i) + 12 > 0x100000000ULL ||
                !call.runtime().memory.read(table + 12U * static_cast<std::uint32_t>(i), entry))
                return "JNIEnv::RegisterNatives method table is unreadable";
            const auto word = [&](unsigned n) {
                std::uint32_t v{};
                for (unsigned b = 0; b < 4; ++b) v |= std::to_integer<std::uint32_t>(entry[4 * n + b]) << (8 * b);
                return v;
            };
            JavaBootstrap::NativeMethod method;
            method.class_name = state->classes[(handle - class_handle_base) / 4 - 1];
            if (word(0) == 0 || !read_guest_string(call.runtime().memory, word(0), max_name, method.name) ||
                word(1) == 0 || !read_guest_string(call.runtime().memory, word(1), max_name, method.signature))
                return "JNIEnv::RegisterNatives method name or signature is unreadable";
            if (word(2) == 0) return "JNIEnv::RegisterNatives function pointer is null";
            method.address = word(2);
            staged.push_back(std::move(method));
        }
        // All-or-nothing: a bad entry leaves earlier entries unregistered.
        for (auto& method : staged) state->natives.push_back(std::move(method));
        call.set_result(0);  // JNI_OK
        return std::nullopt;
    });
    // jmethodID/jfieldID lookups: record (kind, class, name, signature) and return an
    // opaque id. The member's existence is not verified (no Java side yet); an id
    // is only ever consumed by later calls that are themselves explicit services.
    const auto bind_member = [&](const char* function, JavaBootstrap::Member::Kind kind) {
        dispatcher.bind(function, [state, env_check, kind, function](ImportCall& call) -> std::optional<std::string> {
            const std::string label = function;
            if (call.arg(0) != env_check) return label + " called with a foreign JNIEnv pointer";
            const auto handle = call.arg(1);
            if (handle < class_handle_base + 4 || (handle - class_handle_base) % 4 != 0 ||
                (handle - class_handle_base) / 4 > state->classes.size())
                return label + " class reference is not valid";
            JavaBootstrap::Member member;
            member.kind = kind;
            member.class_name = state->classes[(handle - class_handle_base) / 4 - 1];
            if (call.arg(2) == 0 || !read_guest_string(call.runtime().memory, call.arg(2), max_name, member.name) ||
                call.arg(3) == 0 || !read_guest_string(call.runtime().memory, call.arg(3), max_name, member.signature))
                return label + " name or signature is null, unreadable or too long";
            for (std::size_t i = 0; i < state->members.size(); ++i) {
                const auto& known = state->members[i];
                if (known.kind == kind && known.class_name == member.class_name && known.name == member.name &&
                    known.signature == member.signature) {
                    call.set_result(member_handle_base + 4 * static_cast<std::uint32_t>(i + 1));
                    return std::nullopt;
                }
            }
            if (state->members.size() >= max_members) return label + " reference table is full";
            state->members.push_back(std::move(member));
            call.set_result(member_handle_base + 4 * static_cast<std::uint32_t>(state->members.size()));
            return std::nullopt;
        });
    };
    bind_member("JNIEnv::GetMethodID", JavaBootstrap::Member::Kind::method);
    bind_member("JNIEnv::GetStaticMethodID", JavaBootstrap::Member::Kind::static_method);
    bind_member("JNIEnv::GetFieldID", JavaBootstrap::Member::Kind::field);
    bind_member("JNIEnv::GetStaticFieldID", JavaBootstrap::Member::Kind::static_field);
    // NewObject / NewObjectV / NewObjectA(env, clazz, ctor, ...): only the empty
    // java/lang/String via its ()V constructor is modelled. Any other class or
    // constructor stops explicitly instead of producing a fake instance.
    for (const char* function : {"JNIEnv::NewObject", "JNIEnv::NewObjectV", "JNIEnv::NewObjectA"}) {
        dispatcher.bind(function, [state, env_check, function](ImportCall& call) -> std::optional<std::string> {
            const std::string label = function;
            if (call.arg(0) != env_check) return label + " called with a foreign JNIEnv pointer";
            const auto handle = call.arg(1), ctor = call.arg(2);
            if (handle < class_handle_base + 4 || (handle - class_handle_base) % 4 != 0 ||
                (handle - class_handle_base) / 4 > state->classes.size())
                return label + " class reference is not valid";
            if (ctor < member_handle_base + 4 || (ctor - member_handle_base) % 4 != 0 ||
                (ctor - member_handle_base) / 4 > state->members.size())
                return label + " constructor id is not valid";
            const auto& cls = state->classes[(handle - class_handle_base) / 4 - 1];
            const auto& member = state->members[(ctor - member_handle_base) / 4 - 1];
            if (member.kind != JavaBootstrap::Member::Kind::method || member.class_name != cls || member.name != "<init>")
                return label + " method id is not a constructor of the class";
            if (cls != "java/lang/String" || member.signature != "()V")
                return label + " does not model " + cls + "." + member.signature;
            if (state->objects.size() >= max_objects) return label + " object table is full";
            state->objects.push_back(cls);
            call.set_result(object_handle_base + 4 * static_cast<std::uint32_t>(state->objects.size()));
            return std::nullopt;
        });
    }
    const auto referent_ok = [state](std::uint32_t h) {
        const auto in = [h](std::uint32_t base, std::size_t count) { return h >= base + 4 && (h - base) % 4 == 0 && (h - base) / 4 <= count; };
        return in(class_handle_base, state->classes.size()) || in(object_handle_base, state->objects.size());
    };
    // jobject NewGlobalRef(env, obj): a distinct reference to the same class/object.
    dispatcher.bind("JNIEnv::NewGlobalRef", [state, env_check, referent_ok](ImportCall& call) -> std::optional<std::string> {
        if (call.arg(0) != env_check) return "JNIEnv::NewGlobalRef called with a foreign JNIEnv pointer";
        if (call.arg(1) == 0) { call.set_result(0); return std::nullopt; }  // NewGlobalRef(NULL) is NULL
        if (!referent_ok(call.arg(1))) return "JNIEnv::NewGlobalRef reference is not valid";
        if (state->globals.size() >= max_globals) return "JNIEnv::NewGlobalRef reference table is full";
        state->globals.push_back(call.arg(1));
        call.set_result(global_handle_base + 4 * static_cast<std::uint32_t>(state->globals.size()));
        return std::nullopt;
    });
    // void DeleteGlobalRef(env, globalRef): deleting twice or a non-global is an error.
    dispatcher.bind("JNIEnv::DeleteGlobalRef", [state, env_check](ImportCall& call) -> std::optional<std::string> {
        if (call.arg(0) != env_check) return "JNIEnv::DeleteGlobalRef called with a foreign JNIEnv pointer";
        const auto h = call.arg(1);
        if (h == 0) return std::nullopt;
        if (h < global_handle_base + 4 || (h - global_handle_base) % 4 != 0 || (h - global_handle_base) / 4 > state->globals.size() ||
            state->globals[(h - global_handle_base) / 4 - 1] == 0)
            return "JNIEnv::DeleteGlobalRef reference is not a live global reference";
        state->globals[(h - global_handle_base) / 4 - 1] = 0;
        return std::nullopt;
    });
    // void DeleteLocalRef(env, localRef): validates the reference. Class and object
    // table entries are not reclaimed (bounded tables), so deletion changes nothing else.
    dispatcher.bind("JNIEnv::DeleteLocalRef", [env_check, referent_ok](ImportCall& call) -> std::optional<std::string> {
        if (call.arg(0) != env_check) return "JNIEnv::DeleteLocalRef called with a foreign JNIEnv pointer";
        if (call.arg(1) != 0 && !referent_ok(call.arg(1))) return "JNIEnv::DeleteLocalRef reference is not valid";
        return std::nullopt;
    });
    // jint JavaVM::GetEnv(JavaVM* vm, void** penv, jint version)
    const std::uint32_t vm = vm_, env = env_;
    dispatcher.bind("JavaVM::GetEnv", [vm, env](ImportCall& call) -> std::optional<std::string> {
        if (call.arg(0) != vm) return "JavaVM::GetEnv called with a foreign JavaVM pointer";
        if (call.arg(1) == 0) return "JavaVM::GetEnv output pointer is null";
        const auto version = call.arg(2);
        constexpr std::uint32_t supported[] = {0x00010001, 0x00010002, 0x00010004, 0x00010006};
        bool ok = false;
        for (auto v : supported) ok = ok || v == version;
        if (!put_guest(call.runtime().memory, call.arg(1), ok ? env : 0)) return "JavaVM::GetEnv output pointer is not writable";
        call.set_result(ok ? 0U : static_cast<std::uint32_t>(-3));  // JNI_OK / JNI_EVERSION
        return std::nullopt;
    });
}
}
