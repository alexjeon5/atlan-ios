// SPDX-License-Identifier: GPL-2.0-or-later
#include "atlan/opensles.hpp"
#include <array>
#include <cstdio>
#include <map>
#include <stdexcept>

namespace atlan {
namespace {
// SL_IID_* data symbols the guest imports, with the (spec) interface IDs. The
// guest only passes the ID pointers back, so identity is by address.
struct IidSpec { const char* symbol; std::array<std::uint8_t, 16> guid; };
constexpr IidSpec iids[] = {
    {"SL_IID_ENGINE", {0x60, 0xc2, 0x97, 0x8d, 0xd4, 0xdd, 0xdb, 0x11, 0x95, 0x8f, 0x00, 0x02, 0xa5, 0xd5, 0xc5, 0x1b}},
    {"SL_IID_ENVIRONMENTALREVERB", {0x80, 0x9a, 0x6c, 0xc2, 0xd4, 0xdd, 0xdb, 0x11, 0xa7, 0xa1, 0x00, 0x02, 0xa5, 0xd5, 0xc5, 0x1b}},
    {"SL_IID_PLAYBACKRATE", {0x20, 0x1c, 0x2e, 0x2b, 0xd4, 0xdd, 0xdb, 0x11, 0xa6, 0x4a, 0x00, 0x02, 0xa5, 0xd5, 0xc5, 0x1b}},
    {"SL_IID_BUFFERQUEUE", {0xc0, 0x9c, 0xc9, 0x2b, 0xd4, 0xdd, 0xdb, 0x11, 0xac, 0xee, 0x00, 0x02, 0xa5, 0xd5, 0xc5, 0x1b}},
    {"SL_IID_ANDROIDCONFIGURATION", {0xe0, 0xa7, 0xf6, 0x89, 0xac, 0xbe, 0xdf, 0x11, 0x8b, 0x5c, 0x00, 0x02, 0xa5, 0xd5, 0xc5, 0x1b}},
    {"SL_IID_PLAY", {0xc0, 0xd9, 0x0b, 0xef, 0xd4, 0xdd, 0xdb, 0x11, 0xbf, 0x49, 0x00, 0x02, 0xa5, 0xd5, 0xc5, 0x1b}},
    {"SL_IID_EFFECTSEND", {0x80, 0xf3, 0xf1, 0xaa, 0xd4, 0xdd, 0xdb, 0x11, 0xa2, 0x16, 0x00, 0x02, 0xa5, 0xd5, 0xc5, 0x1b}},
    {"SL_IID_VOLUME", {0xe0, 0xed, 0xe8, 0x09, 0xd4, 0xdd, 0xdb, 0x11, 0xa3, 0xa8, 0x00, 0x02, 0xa5, 0xd5, 0xc5, 0x1b}},
    {"SL_IID_SEEK", {0xc0, 0x25, 0x1d, 0x3e, 0xd4, 0xdd, 0xdb, 0x11, 0xa2, 0x32, 0x00, 0x02, 0xa5, 0xd5, 0xc5, 0x1b}},
    {"SL_IID_MUTESOLO", {0xc0, 0x0d, 0x5f, 0x5f, 0xd4, 0xdd, 0xdb, 0x11, 0x8c, 0x45, 0x00, 0x02, 0xa5, 0xd5, 0xc5, 0x1b}},
    {"SL_IID_ANDROIDSIMPLEBUFFERQUEUE", {0x40, 0x49, 0x8e, 0x19, 0xd7, 0xc5, 0xdd, 0x11, 0xad, 0x8f, 0x00, 0x02, 0xa5, 0xd5, 0xc5, 0x1b}},
    {"SL_IID_RECORD", {0x00, 0x2d, 0xb4, 0xc0, 0xd4, 0xdd, 0xdb, 0x11, 0x96, 0xfa, 0x00, 0x02, 0xa5, 0xd5, 0xc5, 0x1b}},
};
constexpr std::uint32_t iid_words_off = 0x000, iid_guid_off = 0x100, vtable_off = 0x400;

struct InterfaceSpec { const char* name; std::vector<const char*> methods; };
const std::vector<InterfaceSpec>& interface_specs() {
    static const std::vector<InterfaceSpec> specs{
        {"SLObjectItf", {"Realize", "Resume", "GetState", "GetInterface", "RegisterCallback", "AbortAsyncOperation",
                         "Destroy", "SetPriority", "GetPriority", "SetLossOfControlInterfaces"}},
        {"SLEngineItf", {"CreateLEDDevice", "CreateVibraDevice", "CreateAudioPlayer", "CreateAudioRecorder",
                         "CreateMidiPlayer", "CreateListener", "Create3DGroup", "CreateOutputMix",
                         "CreateMetadataExtractor", "CreateExtensionObject", "QueryNumSupportedInterfaces",
                         "QuerySupportedInterfaces", "QueryNumSupportedExtensions", "QuerySupportedExtension",
                         "IsExtensionSupported"}},
        {"SLPlayItf", {"SetPlayState", "GetPlayState", "GetDuration", "GetPosition", "RegisterCallback",
                       "SetCallbackEventsMask", "GetCallbackEventsMask", "SetMarkerPosition", "ClearMarkerPosition",
                       "GetMarkerPosition", "SetPositionUpdatePeriod", "GetPositionUpdatePeriod"}},
        {"SLBufferQueueItf", {"Enqueue", "Clear", "GetState", "RegisterCallback"}},
        {"SLAndroidSimpleBufferQueueItf", {"Enqueue", "Clear", "GetState", "RegisterCallback"}},
        {"SLVolumeItf", {"SetVolumeLevel", "GetVolumeLevel", "GetMaxVolumeLevel", "SetMute", "GetMute",
                         "EnableStereoPosition", "IsEnabledStereoPosition", "SetStereoPosition"}},
    };
    return specs;
}
void put32(std::vector<std::byte>& bytes, std::size_t at, std::uint32_t value) {
    for (unsigned i = 0; i < 4; ++i) bytes[at + i] = std::byte((value >> (8 * i)) & 255);
}
bool write32(Memory& memory, std::uint32_t at, std::uint32_t value) {
    std::array<std::byte, 4> bytes{};
    for (unsigned i = 0; i < 4; ++i) bytes[i] = std::byte((value >> (8 * i)) & 255);
    return memory.write(at, bytes);
}
}

// Reads `count` (IID pointer, required flag) pairs; flags are SLboolean = 32-bit.
struct Request { std::uint32_t iid{}; bool required{}; };
std::optional<std::string> read_requests(Memory& memory, std::uint32_t count, std::uint32_t ids, std::uint32_t flags,
                                         std::vector<Request>& out) {
    if (count > 16) return "too many requested interfaces";
    for (std::uint32_t i = 0; i < count; ++i) {
        std::array<std::byte, 4> a{}, b{};
        if (ids == 0 || !memory.read(ids + 4 * i, a)) return "requested interface IDs are unreadable";
        Request r;
        for (unsigned k = 0; k < 4; ++k) r.iid |= std::to_integer<std::uint32_t>(a[k]) << (8 * k);
        if (flags != 0 && memory.read(flags + 4 * i, b)) {
            std::uint32_t v{};
            for (unsigned k = 0; k < 4; ++k) v |= std::to_integer<std::uint32_t>(b[k]) << (8 * k);
            r.required = v != 0;
        } else if (flags != 0) return "requested interface flags are unreadable";
        out.push_back(r);
    }
    return std::nullopt;
}
enum Iface : std::uint8_t { i_object, i_engine, i_play, i_bufferqueue, i_androidbq, i_volume, iface_count };
constexpr std::uint32_t sl_ok = 0, sl_param_invalid = 2, sl_feature_unsupported = 12, sl_resource_error = 5, sl_memory_failure = 9;

struct OpenSles::State {
    enum class Type : std::uint8_t { engine, output_mix, audio_player };
    struct Object {
        Type type{};
        bool realized{}, destroyed{};
        std::array<std::uint32_t, iface_count> faces{};  // guest address per exposed interface, 0 = none
    };
    struct Face { std::uint32_t object{}; Iface kind{}; };
    Config config;
    std::uint32_t slot_first[iface_count]{};
    std::vector<Object> objects;
    std::map<std::uint32_t, Face> faces;  // interface word address -> owner
    std::uint32_t next_word{};
    void trace(const std::string& line) const { if (config.trace) config.trace(line); }
    std::uint32_t vtable(Iface kind) const { return config.data_base + vtable_off + 4 * slot_first[kind]; }
    // Allocates one guest word holding the vtable pointer of `kind` for object `object`.
    std::uint32_t new_face(Memory& memory, std::uint32_t object, Iface kind) {
        const std::uint32_t limit = config.object_base + config.object_pages * Memory::page_size;
        if (next_word + 8 > limit || faces.size() >= 4096) return 0;
        const std::uint32_t at = next_word;
        if (!write32(memory, at, vtable(kind))) return 0;
        next_word += 8;
        faces.emplace(at, Face{object, kind});
        objects[object].faces[kind] = at;
        return at;
    }
    std::optional<std::string> face(std::uint32_t at, Iface kind, const char* what, Face*& out) {
        const auto it = faces.find(at);
        if (it == faces.end() || it->second.kind != kind || objects[it->second.object].destroyed)
            return std::string(what) + " called with an invalid or destroyed interface pointer";
        out = &it->second;
        return std::nullopt;
    }
    Iface iid_kind(std::uint32_t iid_pointer) const {  // iface_count if unknown
        // IID pointers are the GUID blocks in the data page.
        const std::uint32_t base = config.data_base + iid_guid_off;
        if (iid_pointer < base || (iid_pointer - base) % 16 != 0 || (iid_pointer - base) / 16 >= std::size(iids)) return iface_count;
        const std::string_view name = iids[(iid_pointer - base) / 16].symbol;
        if (name == "SL_IID_ENGINE") return i_engine;
        if (name == "SL_IID_PLAY") return i_play;
        if (name == "SL_IID_BUFFERQUEUE") return i_bufferqueue;
        if (name == "SL_IID_ANDROIDSIMPLEBUFFERQUEUE") return i_androidbq;
        if (name == "SL_IID_VOLUME") return i_volume;
        return iface_count;
    }
    std::string iid_name(std::uint32_t iid_pointer) const {
        const std::uint32_t base = config.data_base + iid_guid_off;
        if (iid_pointer < base || (iid_pointer - base) % 16 != 0 || (iid_pointer - base) / 16 >= std::size(iids)) return "unknown IID";
        return iids[(iid_pointer - base) / 16].symbol;
    }
};

std::optional<std::uint32_t> OpenSles::data_symbol(std::string_view name, std::uint32_t data_base) {
    for (std::size_t i = 0; i < std::size(iids); ++i)
        if (name == iids[i].symbol) return data_base + iid_words_off + 4 * static_cast<std::uint32_t>(i);
    return std::nullopt;
}

OpenSles::~OpenSles() = default;

OpenSles::OpenSles(Runtime& runtime, ImportDispatcher& dispatcher, Config config) : state_(std::make_shared<State>()) {
    std::size_t slots = 0;
    for (const auto& spec : interface_specs()) slots += spec.methods.size();
    const std::uint32_t table_bytes = vtable_off + 4 * static_cast<std::uint32_t>(slots);
    const std::uint32_t table_pages = (table_bytes + Memory::page_size - 1) / Memory::page_size;
    if (config.data_base % Memory::page_size != 0 || config.object_base % Memory::page_size != 0 ||
        config.trap_base % 4 != 0 || config.trap_base == 0 || config.object_pages == 0 ||
        runtime.memory.translate(config.data_base, MemoryAccess::read) != nullptr ||
        runtime.memory.translate(config.object_base, MemoryAccess::read) != nullptr ||
        runtime.memory.translate(config.trap_base, MemoryAccess::read) != nullptr)
        throw std::invalid_argument("invalid OpenSL ES layout");
    std::vector<std::byte> data(std::size_t{table_pages} * Memory::page_size);
    for (std::size_t i = 0; i < std::size(iids); ++i) {
        put32(data, iid_words_off + 4 * i, config.data_base + iid_guid_off + 16 * static_cast<std::uint32_t>(i));
        for (unsigned b = 0; b < 16; ++b) data[iid_guid_off + 16 * i + b] = std::byte(iids[i].guid[b]);
    }
    const auto state = state_;
    state->config = config;
    state->next_word = config.object_base;
    std::uint32_t slot = 0;
    for (std::size_t k = 0; k < interface_specs().size(); ++k) {
        const auto& spec = interface_specs()[k];
        state->slot_first[k] = slot;
        for (const char* method : spec.methods) {
            const std::uint32_t trap = config.trap_base + 4 * slot;
            put32(data, vtable_off + 4 * slot, trap);
            (void)dispatcher.bind_trap(trap, std::string(spec.name) + "::" + method);
            ++slot;
        }
    }
    runtime.memory.map(config.data_base, data.size(), Permissions::read, data);
    runtime.memory.map(config.object_base, std::uint64_t{config.object_pages} * Memory::page_size, Permissions::read_write);

    const auto hex = [](std::uint32_t v) { char b[16]; std::snprintf(b, sizeof b, "0x%x", v); return std::string(b); };
    // SLresult slCreateEngine(SLObjectItf* pEngine, SLuint32 numOptions, const SLEngineOption* pEngineOptions,
    //                         SLuint32 numInterfaces, const SLInterfaceID* pInterfaceIds, const SLboolean* pInterfaceRequired)
    dispatcher.bind("slCreateEngine", [state, hex](ImportCall& call) -> std::optional<std::string> {
        std::uint32_t num_interfaces{}, ids{}, required{};
        if (!call.read_arg(3, num_interfaces) || !call.read_arg(4, ids) || !call.read_arg(5, required))
            return "slCreateEngine arguments are unreadable";
        state->trace("slCreateEngine out=" + hex(call.arg(0)) + " options=" + std::to_string(call.arg(1)) + "@" + hex(call.arg(2)) +
                     " interfaces=" + std::to_string(num_interfaces));
        if (call.arg(0) == 0) { call.set_result(sl_param_invalid); return std::nullopt; }
        // SLEngineOption is {SLuint32 feature; SLuint32 data}. Only SL_ENGINEOPTION_THREADSAFE (1) is accepted;
        // the engine here is single-threaded on the guest thread, so the flag has no effect.
        for (std::uint32_t i = 0; i < call.arg(1); ++i) {
            std::array<std::byte, 8> raw{};
            if (i >= 8 || !call.runtime().memory.read(call.arg(2) + 8 * i, raw)) return "slCreateEngine options are unreadable or too many";
            std::uint32_t feature{}, value{};
            for (unsigned b = 0; b < 4; ++b) {
                feature |= std::to_integer<std::uint32_t>(raw[b]) << (8 * b);
                value |= std::to_integer<std::uint32_t>(raw[4 + b]) << (8 * b);
            }
            state->trace("  option feature=" + hex(feature) + " data=" + hex(value));
            if (feature != 1) return "slCreateEngine option " + hex(feature) + " is not implemented";
        }
        if (num_interfaces != 0) return "slCreateEngine with explicit interfaces is not implemented";
        state->objects.push_back({State::Type::engine, false, false, {}});
        const auto object = static_cast<std::uint32_t>(state->objects.size() - 1);
        const auto at = state->new_face(call.runtime().memory, object, i_object);
        if (at == 0 || !write32(call.runtime().memory, call.arg(0), at)) return "slCreateEngine cannot publish the engine object";
        call.set_result(sl_ok);
        return std::nullopt;
    });
    // SLresult Realize(SLObjectItf self, SLboolean async)
    dispatcher.bind("SLObjectItf::Realize", [state](ImportCall& call) -> std::optional<std::string> {
        State::Face* f{};
        if (auto e = state->face(call.arg(0), i_object, "SLObjectItf::Realize", f)) return e;
        state->trace("Object::Realize async=" + std::to_string(call.arg(1)));
        if (call.arg(1) != 0) return "SLObjectItf::Realize with async=true is not implemented";
        state->objects[f->object].realized = true;
        call.set_result(sl_ok);
        return std::nullopt;
    });
    // SLresult GetInterface(SLObjectItf self, const SLInterfaceID iid, void* pInterface)
    dispatcher.bind("SLObjectItf::GetInterface", [state](ImportCall& call) -> std::optional<std::string> {
        State::Face* f{};
        if (auto e = state->face(call.arg(0), i_object, "SLObjectItf::GetInterface", f)) return e;
        const auto kind = state->iid_kind(call.arg(1));
        state->trace("Object::GetInterface " + state->iid_name(call.arg(1)) + " object=" + std::to_string(f->object));
        auto& object = state->objects[f->object];
        if (!object.realized) return "SLObjectItf::GetInterface before Realize";
        if (state->iid_name(call.arg(1)) == "unknown IID") return "SLObjectItf::GetInterface with an unknown interface ID";
        if (kind == iface_count || (kind != i_engine && object.faces[kind] == 0 && object.type == State::Type::output_mix)) {
            // Known interface that this object does not expose: the spec result, not a stop.
            if (!write32(call.runtime().memory, call.arg(2), 0)) return "SLObjectItf::GetInterface cannot write the interface pointer";
            call.set_result(sl_feature_unsupported);
            return std::nullopt;
        }
        if (kind == i_engine && object.type != State::Type::engine)
            return "SLObjectItf::GetInterface SL_IID_ENGINE on a non-engine object";
        if (kind != i_engine) return "SLObjectItf::GetInterface of " + state->iid_name(call.arg(1)) + " is not implemented for this object";
        auto at = object.faces[kind];
        if (at == 0) at = state->new_face(call.runtime().memory, f->object, kind);
        if (at == 0 || !write32(call.runtime().memory, call.arg(2), at)) return "SLObjectItf::GetInterface cannot publish the interface";
        call.set_result(sl_ok);
        return std::nullopt;
    });
    // SLresult CreateOutputMix(SLEngineItf self, SLObjectItf* pMix, SLuint32 numInterfaces,
    //                          const SLInterfaceID* pInterfaceIds, const SLboolean* pInterfaceRequired)
    dispatcher.bind("SLEngineItf::CreateOutputMix", [state, hex](ImportCall& call) -> std::optional<std::string> {
        State::Face* f{};
        if (auto e = state->face(call.arg(0), i_engine, "SLEngineItf::CreateOutputMix", f)) return e;
        std::uint32_t required_flags{};
        if (!call.read_arg(4, required_flags)) return "SLEngineItf::CreateOutputMix arguments are unreadable";
        std::vector<Request> requests;
        if (auto e = read_requests(call.runtime().memory, call.arg(2), call.arg(3), required_flags, requests)) return "SLEngineItf::CreateOutputMix: " + *e;
        for (const auto& r : requests)
            state->trace("  CreateOutputMix requests " + state->iid_name(r.iid) + (r.required ? " (required)" : " (optional)"));
        // Optional interfaces the host does not provide are simply not exposed (the spec's
        // behaviour: GetInterface later reports SL_RESULT_FEATURE_UNSUPPORTED). A required
        // one fails creation with the same code.
        for (const auto& r : requests)
            if (r.required) { call.set_result(sl_feature_unsupported); return std::nullopt; }
        if (call.arg(1) == 0) { call.set_result(sl_param_invalid); return std::nullopt; }
        state->objects.push_back({State::Type::output_mix, false, false, {}});
        const auto object = static_cast<std::uint32_t>(state->objects.size() - 1);
        const auto at = state->new_face(call.runtime().memory, object, i_object);
        if (at == 0 || !write32(call.runtime().memory, call.arg(1), at)) return "SLEngineItf::CreateOutputMix cannot publish the object";
        call.set_result(sl_ok);
        return std::nullopt;
    });
    (void)sl_resource_error; (void)sl_memory_failure;
}
}
