// SPDX-License-Identifier: GPL-2.0-or-later
#include "atlan/opensles.hpp"
#include <array>
#include <cstdio>
#include <cmath>
#include <deque>
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
std::string hex_value(std::uint32_t v) { char b[16]; std::snprintf(b, sizeof b, "%x", v); return b; }
constexpr std::uint32_t sl_ok = 0, sl_param_invalid = 2, sl_feature_unsupported = 12, sl_buffer_insufficient = 7, sl_resource_error = 5, sl_memory_failure = 9;

struct OpenSles::State {
    enum class Type : std::uint8_t { engine, output_mix, audio_player };
    struct Pending { std::uint64_t end{}; };  // stream offset at which an enqueued buffer is finished
    struct Player {
        PcmFormat format{};
        Iface queue_kind{};
        std::uint32_t num_buffers{};
        bool has_volume{};
        std::unique_ptr<AudioStream> stream;
        std::deque<Pending> pending;
        std::uint64_t appended{}, completed{};
        std::uint32_t play_state{1};  // SL_PLAYSTATE_STOPPED
        std::uint32_t callback{}, context{};
        std::int16_t millibel{0};
        bool mute{};
    };
    struct Object {
        Type type{};
        bool realized{}, destroyed{};
        std::array<std::uint32_t, iface_count> faces{};  // guest address per exposed interface, 0 = none
        std::unique_ptr<Player> player;
    };
    struct Face { std::uint32_t object{}; Iface kind{}; };
    Config config;
    ImportDispatcher* dispatcher{};
    std::uint32_t slot_first[iface_count]{};
    std::deque<Object> objects;  // stable addresses
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

// The bound services share State and may outlive this object inside the dispatcher.
// Streams belong to the sink, so they are closed here and every interface is
// forgotten: later guest calls on them fail explicitly instead of touching a dead sink.
OpenSles::~OpenSles() {
    state_->faces.clear();
    state_->objects.clear();
}

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
    state->dispatcher = &dispatcher;
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
        state->objects.emplace_back().type = State::Type::engine;
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
        auto& object = state->objects[f->object];
        if (object.realized) { call.set_result(sl_ok); return std::nullopt; }  // spec: Realize on a realized object is a precondition violation; keep idempotent
        if (object.player) {
            if (state->config.sink == nullptr) return "SLObjectItf::Realize of an audio player but no audio sink is configured";
            std::string error;
            object.player->stream = state->config.sink->open(object.player->format, error);
            if (!object.player->stream) {
                state->trace("  sink refused the stream: " + error);
                call.set_result(sl_resource_error);
                return std::nullopt;
            }
            object.player->stream->set_gain(1.0F);
        }
        object.realized = true;
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
        if (!object.realized) { call.set_result(3); return std::nullopt; }  // SL_RESULT_PRECONDITIONS_VIOLATED
        if (state->iid_name(call.arg(1)) == "unknown IID") return "SLObjectItf::GetInterface with an unknown interface ID";
        std::uint32_t at = 0;
        if (kind != iface_count) {
            at = object.faces[kind];
            if (at == 0 && kind == i_engine && object.type == State::Type::engine) at = state->new_face(call.runtime().memory, f->object, kind);
        }
        if (kind == iface_count || (at == 0 && kind != i_engine)) {
            // A known interface this object does not expose: the spec's result, not a stop.
            if (!write32(call.runtime().memory, call.arg(2), 0)) return "SLObjectItf::GetInterface cannot write the interface pointer";
            call.set_result(sl_feature_unsupported);
            return std::nullopt;
        }
        if (at == 0 || !write32(call.runtime().memory, call.arg(2), at)) return "SLObjectItf::GetInterface cannot publish the interface";
        call.set_result(sl_ok);
        return std::nullopt;
    });
    // SLresult CreateOutputMix(SLEngineItf self, SLObjectItf* pMix, SLuint32 numInterfaces,
    //                          const SLInterfaceID* pInterfaceIds, const SLboolean* pInterfaceRequired)
    dispatcher.bind("SLEngineItf::CreateOutputMix", [state](ImportCall& call) -> std::optional<std::string> {
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
        state->objects.emplace_back().type = State::Type::output_mix;
        const auto object = static_cast<std::uint32_t>(state->objects.size() - 1);
        const auto at = state->new_face(call.runtime().memory, object, i_object);
        if (at == 0 || !write32(call.runtime().memory, call.arg(1), at)) return "SLEngineItf::CreateOutputMix cannot publish the object";
        call.set_result(sl_ok);
        return std::nullopt;
    });

    // SLresult GetState(SLObjectItf self, SLuint32* pState): 1 unrealized, 2 realized
    dispatcher.bind("SLObjectItf::GetState", [state](ImportCall& call) -> std::optional<std::string> {
        State::Face* f{};
        if (auto e = state->face(call.arg(0), i_object, "SLObjectItf::GetState", f)) return e;
        if (!write32(call.runtime().memory, call.arg(1), state->objects[f->object].realized ? 2 : 1)) return "SLObjectItf::GetState cannot write the state";
        call.set_result(sl_ok);
        return std::nullopt;
    });
    // void Destroy(SLObjectItf self)
    dispatcher.bind("SLObjectItf::Destroy", [state](ImportCall& call) -> std::optional<std::string> {
        State::Face* f{};
        if (auto e = state->face(call.arg(0), i_object, "SLObjectItf::Destroy", f)) return e;
        state->trace("Object::Destroy object=" + std::to_string(f->object));
        auto& object = state->objects[f->object];
        object.destroyed = true;
        object.player.reset();  // closes the sink stream
        return std::nullopt;
    });
    // SLresult CreateAudioPlayer(SLEngineItf self, SLObjectItf* pPlayer, SLDataSource* pAudioSrc, SLDataSink* pAudioSnk,
    //     SLuint32 numInterfaces, const SLInterfaceID* pInterfaceIds, const SLboolean* pInterfaceRequired)
    dispatcher.bind("SLEngineItf::CreateAudioPlayer", [state, hex](ImportCall& call) -> std::optional<std::string> {
        State::Face* f{};
        if (auto e = state->face(call.arg(0), i_engine, "SLEngineItf::CreateAudioPlayer", f)) return e;
        std::uint32_t count{}, ids{}, flags{};
        if (!call.read_arg(4, count) || !call.read_arg(5, ids) || !call.read_arg(6, flags)) return "SLEngineItf::CreateAudioPlayer arguments are unreadable";
        auto& memory = call.runtime().memory;
        const auto word = [&](std::uint32_t at, std::uint32_t& out) {
            std::array<std::byte, 4> b{};
            if (!memory.read(at, b)) return false;
            out = 0;
            for (unsigned k = 0; k < 4; ++k) out |= std::to_integer<std::uint32_t>(b[k]) << (8 * k);
            return true;
        };
        // SLDataSource / SLDataSink are {void* locator; void* format}.
        std::uint32_t src_loc{}, src_fmt{}, snk_loc{}, snk_fmt{};
        if (call.arg(2) == 0 || call.arg(3) == 0 || !word(call.arg(2), src_loc) || !word(call.arg(2) + 4, src_fmt) ||
            !word(call.arg(3), snk_loc) || !word(call.arg(3) + 4, snk_fmt) || src_loc == 0 || snk_loc == 0)
            return "SLEngineItf::CreateAudioPlayer data source or sink is unreadable";
        std::uint32_t loc_type{}, num_buffers{}, snk_type{}, mix_object{};
        if (!word(src_loc, loc_type) || !word(src_loc + 4, num_buffers) || !word(snk_loc, snk_type) || !word(snk_loc + 4, mix_object))
            return "SLEngineItf::CreateAudioPlayer locators are unreadable";
        Iface queue_kind{};
        if (loc_type == 0x800007bd) queue_kind = i_androidbq;       // SL_DATALOCATOR_ANDROIDSIMPLEBUFFERQUEUE
        else if (loc_type == 6) queue_kind = i_bufferqueue;         // SL_DATALOCATOR_BUFFERQUEUE
        else return "SLEngineItf::CreateAudioPlayer source locator " + hex(loc_type) + " is not implemented";
        if (snk_type != 3) return "SLEngineItf::CreateAudioPlayer sink locator " + hex(snk_type) + " is not implemented";  // SL_DATALOCATOR_OUTPUTMIX
        if (num_buffers == 0 || num_buffers > 64) return "SLEngineItf::CreateAudioPlayer buffer count " + std::to_string(num_buffers) + " is not accepted";
        // SLDataFormat_PCM {formatType, numChannels, samplesPerSec (milliHz), bitsPerSample, containerSize, channelMask, endianness}
        std::uint32_t fmt[7]{};
        for (unsigned k = 0; k < 7; ++k)
            if (src_fmt == 0 || !word(src_fmt + 4 * k, fmt[k])) return "SLEngineItf::CreateAudioPlayer PCM format is unreadable";
        state->trace("  CreateAudioPlayer queue=" + std::string(queue_kind == i_androidbq ? "android-simple" : "buffer") + " buffers=" + std::to_string(num_buffers) +
                     " format=" + std::to_string(fmt[0]) + " channels=" + std::to_string(fmt[1]) + " mHz=" + std::to_string(fmt[2]) +
                     " bits=" + std::to_string(fmt[3]) + " container=" + std::to_string(fmt[4]) + " mask=" + hex(fmt[5]) + " endian=" + std::to_string(fmt[6]));
        if (fmt[0] != 2) return "SLEngineItf::CreateAudioPlayer data format " + std::to_string(fmt[0]) + " is not PCM";
        if (fmt[1] < 1 || fmt[1] > 2 || fmt[3] != 16 || fmt[4] != 16 || fmt[6] != 2)
            return "SLEngineItf::CreateAudioPlayer PCM layout is not implemented (only 16-bit little-endian mono/stereo)";
        if (fmt[2] < 1000 || fmt[2] % 1000 != 0 || fmt[2] > 192000000) return "SLEngineItf::CreateAudioPlayer sample rate is not a whole Hz value";
        if (mix_object == 0 || state->faces.find(mix_object) == state->faces.end() ||
            state->objects[state->faces[mix_object].object].type != State::Type::output_mix)
            return "SLEngineItf::CreateAudioPlayer sink output mix is not a valid object";
        std::vector<Request> requests;
        if (auto e = read_requests(memory, count, ids, flags, requests)) return "SLEngineItf::CreateAudioPlayer: " + *e;
        bool want_volume = false, unsupported_required = false;
        for (const auto& r : requests) {
            const auto kind = state->iid_kind(r.iid);
            state->trace("  CreateAudioPlayer requests " + state->iid_name(r.iid) + (r.required ? " (required)" : " (optional)"));
            if (kind == queue_kind) continue;
            if (kind == i_volume) { want_volume = true; continue; }
            if (r.required) unsupported_required = true;
        }
        if (unsupported_required) { call.set_result(sl_feature_unsupported); return std::nullopt; }
        if (call.arg(1) == 0) { call.set_result(sl_param_invalid); return std::nullopt; }
        state->objects.emplace_back().type = State::Type::audio_player;
        const auto object = static_cast<std::uint32_t>(state->objects.size() - 1);
        auto player = std::make_unique<State::Player>();
        player->format = {fmt[2] / 1000, static_cast<std::uint16_t>(fmt[1])};
        player->queue_kind = queue_kind;
        player->num_buffers = num_buffers;
        player->has_volume = want_volume;
        state->objects[object].player = std::move(player);
        const auto at = state->new_face(memory, object, i_object);
        bool ok = at != 0 && state->new_face(memory, object, i_play) != 0 && state->new_face(memory, object, queue_kind) != 0;
        if (want_volume) ok = ok && state->new_face(memory, object, i_volume) != 0;
        if (!ok || !write32(memory, call.arg(1), at)) return "SLEngineItf::CreateAudioPlayer cannot publish the player";
        call.set_result(sl_ok);
        return std::nullopt;
    });
    // Realize of a player opens the sink stream (device errors are an API failure, not a stop).
    // (Replaces the generic Realize below for players.)

    // Looks up the realized player behind an interface word.
    const auto player_of = [state](ImportCall& call, Iface kind, const char* what, State::Player*& out) -> std::optional<std::string> {
        State::Face* f{};
        if (auto e = state->face(call.arg(0), kind, what, f)) return e;
        auto& object = state->objects[f->object];
        if (!object.player || !object.realized || !object.player->stream) return std::string(what) + " on a player that is not realized";
        out = object.player.get();
        return std::nullopt;
    };
    // SLresult SetPlayState(SLPlayItf self, SLuint32 state): 1 stopped, 2 paused, 3 playing
    dispatcher.bind("SLPlayItf::SetPlayState", [state, player_of](ImportCall& call) -> std::optional<std::string> {
        State::Player* p{};
        if (auto e = player_of(call, i_play, "SLPlayItf::SetPlayState", p)) return e;
        state->trace("Play::SetPlayState " + std::to_string(call.arg(1)));
        if (call.arg(1) < 1 || call.arg(1) > 3) { call.set_result(sl_param_invalid); return std::nullopt; }
        p->play_state = call.arg(1);
        p->stream->set_playing(p->play_state == 3);
        call.set_result(sl_ok);
        return std::nullopt;
    });
    // SLresult GetPlayState(SLPlayItf self, SLuint32* pState)
    dispatcher.bind("SLPlayItf::GetPlayState", [player_of](ImportCall& call) -> std::optional<std::string> {
        State::Player* p{};
        if (auto e = player_of(call, i_play, "SLPlayItf::GetPlayState", p)) return e;
        if (!write32(call.runtime().memory, call.arg(1), p->play_state)) return "SLPlayItf::GetPlayState cannot write the state";
        call.set_result(sl_ok);
        return std::nullopt;
    });
    // Buffer queue: both interfaces have the same layout and semantics here.
    for (const auto kind : {i_bufferqueue, i_androidbq}) {
        const std::string prefix = kind == i_bufferqueue ? "SLBufferQueueItf::" : "SLAndroidSimpleBufferQueueItf::";
        // SLresult Enqueue(self, const void* pBuffer, SLuint32 size)
        dispatcher.bind(prefix + "Enqueue", [state, player_of, kind, prefix](ImportCall& call) -> std::optional<std::string> {
            State::Player* p{};
            if (auto e = player_of(call, kind, (prefix + "Enqueue").c_str(), p)) return e;
            const auto size = call.arg(2);
            if (call.arg(1) == 0 || size == 0 || size % p->format.frame_bytes() != 0 || size > (1U << 24)) {
                call.set_result(sl_param_invalid);
                return std::nullopt;
            }
            if (p->pending.size() >= p->num_buffers) { call.set_result(sl_buffer_insufficient); return std::nullopt; }
            std::vector<std::byte> pcm(size);
            if (!call.runtime().memory.read(call.arg(1), pcm)) return prefix + "Enqueue buffer is unreadable guest memory";
            if (!p->stream->append(pcm)) { call.set_result(sl_resource_error); return std::nullopt; }
            p->appended += size;
            p->pending.push_back({p->appended});
            state->trace("Queue::Enqueue " + std::to_string(size) + " bytes, pending=" + std::to_string(p->pending.size()));
            call.set_result(sl_ok);
            return std::nullopt;
        });
        // SLresult Clear(self): drops everything not yet played, without callbacks
        dispatcher.bind(prefix + "Clear", [state, player_of, kind, prefix](ImportCall& call) -> std::optional<std::string> {
            State::Player* p{};
            if (auto e = player_of(call, kind, (prefix + "Clear").c_str(), p)) return e;
            state->trace("Queue::Clear dropping " + std::to_string(p->pending.size()) + " buffers");
            p->stream->flush();
            p->pending.clear();
            p->appended = p->stream->played_bytes();
            call.set_result(sl_ok);
            return std::nullopt;
        });
        // SLresult GetState(self, SLBufferQueueState* / SLAndroidSimpleBufferQueueState*): {count, playIndex}
        dispatcher.bind(prefix + "GetState", [player_of, kind, prefix](ImportCall& call) -> std::optional<std::string> {
            State::Player* p{};
            if (auto e = player_of(call, kind, (prefix + "GetState").c_str(), p)) return e;
            if (!write32(call.runtime().memory, call.arg(1), static_cast<std::uint32_t>(p->pending.size())) ||
                !write32(call.runtime().memory, call.arg(1) + 4, static_cast<std::uint32_t>(p->completed)))
                return prefix + "GetState cannot write the state";
            call.set_result(sl_ok);
            return std::nullopt;
        });
        // SLresult RegisterCallback(self, callback, void* pContext)
        dispatcher.bind(prefix + "RegisterCallback", [state, player_of, kind, prefix](ImportCall& call) -> std::optional<std::string> {
            State::Player* p{};
            if (auto e = player_of(call, kind, (prefix + "RegisterCallback").c_str(), p)) return e;
            p->callback = call.arg(1);
            p->context = call.arg(2);
            state->trace("Queue::RegisterCallback fn=0x" + hex_value(call.arg(1)));
            call.set_result(sl_ok);
            return std::nullopt;
        });
    }
    // SLresult SetVolumeLevel(SLVolumeItf self, SLmillibel level): level is a signed 16-bit value
    dispatcher.bind("SLVolumeItf::SetVolumeLevel", [state, player_of](ImportCall& call) -> std::optional<std::string> {
        State::Player* p{};
        if (auto e = player_of(call, i_volume, "SLVolumeItf::SetVolumeLevel", p)) return e;
        const auto level = static_cast<std::int16_t>(call.arg(1) & 0xffff);
        if (level < -9600 || level > 0) { call.set_result(sl_param_invalid); return std::nullopt; }  // [min, max]; max is 0 dB here
        p->millibel = level;
        p->stream->set_gain(p->mute ? 0.0F : std::pow(10.0F, static_cast<float>(level) / 2000.0F));
        state->trace("Volume::SetVolumeLevel " + std::to_string(level) + " mB");
        call.set_result(sl_ok);
        return std::nullopt;
    });
    // SLresult GetMaxVolumeLevel(self, SLmillibel* pMax): 0 dB (no amplification)
    dispatcher.bind("SLVolumeItf::GetMaxVolumeLevel", [player_of](ImportCall& call) -> std::optional<std::string> {
        State::Player* p{};
        if (auto e = player_of(call, i_volume, "SLVolumeItf::GetMaxVolumeLevel", p)) return e;
        const std::array<std::byte, 2> zero{};
        if (!call.runtime().memory.write(call.arg(1), zero)) return "SLVolumeItf::GetMaxVolumeLevel cannot write the level";
        call.set_result(sl_ok);
        return std::nullopt;
    });
    // SLresult SetMute(self, SLboolean)
    dispatcher.bind("SLVolumeItf::SetMute", [player_of](ImportCall& call) -> std::optional<std::string> {
        State::Player* p{};
        if (auto e = player_of(call, i_volume, "SLVolumeItf::SetMute", p)) return e;
        p->mute = call.arg(1) != 0;
        p->stream->set_gain(p->mute ? 0.0F : std::pow(10.0F, static_cast<float>(p->millibel) / 2000.0F));
        call.set_result(sl_ok);
        return std::nullopt;
    });
    (void)sl_resource_error; (void)sl_memory_failure;
}

OpenSles::PumpResult OpenSles::pump(Runtime& runtime, std::uint32_t stack_top, std::uint32_t return_target, std::uint64_t budget) {
    PumpResult result;
    auto& st = *state_;
    for (std::size_t i = 0; i < st.objects.size(); ++i) {
        for (;;) {
            auto& object = st.objects[i];
            if (object.destroyed || !object.player || !object.player->stream) break;
            auto& p = *object.player;
            if (p.pending.empty() || p.pending.front().end > p.stream->played_bytes()) break;
            p.pending.pop_front();
            ++p.completed;
            if (p.callback == 0) continue;
            const std::uint32_t caller = object.faces[p.queue_kind];
            const auto outcome = st.dispatcher->call(runtime, p.callback, std::array<std::uint32_t, 2>{caller, p.context}, stack_top, return_target, budget);
            ++result.callbacks;
            if (outcome.stop != Stop::returned) {
                result.error = "buffer-queue callback did not return" + (outcome.message.empty() ? std::string() : ": " + outcome.message) +
                               (outcome.import.empty() ? std::string() : " (" + outcome.import + ")");
                return result;
            }
        }
    }
    return result;
}
std::size_t OpenSles::live_players() const {
    std::size_t n = 0;
    for (const auto& o : state_->objects) n += (!o.destroyed && o.player) ? 1 : 0;
    return n;
}
}
