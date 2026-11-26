// SPDX-License-Identifier: GPL-2.0-or-later
#include "atlan/audio.hpp"
#include "atlan/opensles.hpp"
#include "atlan/threads.hpp"
#include "atlan/wav.hpp"
#include <filesystem>
#include <fstream>
#include <unistd.h>
#include <array>
#include <cstring>
#include <iostream>
#include <stdexcept>

namespace {
unsigned checks{};
void check(bool result, const char* message) {
    ++checks;
    if (!result) throw std::runtime_error(message);
}
using atlan::OpenSles;
constexpr std::uint32_t code = 0x1000, stack = 0x80000, sentinel = 0xffff0000, data = 0x4000, create_engine = 0xf0000000;

struct Fixture {
    atlan::Runtime runtime;
    atlan::ImportDispatcher dispatcher;
    atlan::RecordingAudioSink sink;
    std::optional<OpenSles> sles;
    std::vector<std::string> trace;
    explicit Fixture(bool with_sink = true) {
        runtime.memory.map(code, 0x2000, atlan::Permissions::read_write);
        runtime.memory.map(data, 0x4000, atlan::Permissions::read_write);
        runtime.memory.map(stack - 0x1000, 0x1000, atlan::Permissions::read_write);
        // 0x1000: callback(r0 = caller, r1 = counter): ++counter[0]; counter[1] = caller
        const std::uint32_t cb[] = {0xe5912000, 0xe2822001, 0xe5812000, 0xe5810004, 0xe12fff1e};
        for (unsigned i = 0; i < 5; ++i) put32(0x1000 + 4 * i, cb[i]);
        runtime.memory.protect(code, 0x2000, atlan::Permissions::read_execute);
        OpenSles::Config config;
        if (with_sink) config.sink = &sink;
        config.trace = [this](const std::string& line) { trace.push_back(line); };
        dispatcher.bind_trap(create_engine, "slCreateEngine");
        sles.emplace(runtime, dispatcher, config);
    }
    void put32(std::uint32_t at, std::uint32_t value) {
        std::array<std::byte, 4> bytes{};
        for (unsigned i = 0; i < 4; ++i) bytes[i] = std::byte((value >> (8 * i)) & 255);
        check(runtime.memory.write(at, bytes), "write guest word");
    }
    std::uint32_t get32(std::uint32_t at) {
        std::array<std::byte, 4> bytes{};
        check(runtime.memory.read(at, bytes), "read guest word");
        std::uint32_t value{};
        for (unsigned i = 0; i < 4; ++i) value |= std::to_integer<std::uint32_t>(bytes[i]) << (8 * i);
        return value;
    }
    std::uint32_t iid(const char* name) {  // the SLInterfaceID value the guest loads from the data symbol
        const auto symbol = OpenSles::data_symbol(name);
        check(symbol.has_value(), "iid symbol exists");
        return get32(*symbol);
    }
    atlan::CallOutcome call(std::uint32_t target, std::initializer_list<std::uint32_t> args, std::uint64_t budget = 10000) {
        const std::vector<std::uint32_t> values(args);
        return dispatcher.call(runtime, target, values, stack, sentinel, budget);
    }
    // Calls method `slot` of the interface at `self` through its guest vtable: ldr r12,[r0]; ldr r12,[r12,#4*slot]; bx r12.
    atlan::CallOutcome method(std::uint32_t self, unsigned slot, std::initializer_list<std::uint32_t> args) {
        runtime.memory.protect(code, 0x2000, atlan::Permissions::read_write);
        put32(0x1100, 0xe590c000);
        put32(0x1104, 0xe59cc000 | (4 * slot));
        put32(0x1108, 0xe12fff1c);
        runtime.memory.protect(code, 0x2000, atlan::Permissions::read_execute);
        std::vector<std::uint32_t> values{self};
        values.insert(values.end(), args);
        return dispatcher.call(runtime, 0x1100, values, stack, sentinel, 10000);
    }
    std::uint32_t result() { return static_cast<std::uint32_t>(runtime.cpu.registers[0]); }
};
// Method slots within their vtables.
namespace slot {
constexpr unsigned realize = 0, get_state = 2, get_interface = 3, destroy = 6;
constexpr unsigned create_audio_player = 2, create_output_mix = 7, create_midi = 4;
constexpr unsigned set_play_state = 0, get_play_state = 1, get_duration = 2;
constexpr unsigned enqueue = 0, clear = 1, queue_state = 2, register_callback = 3;
constexpr unsigned set_volume = 0, get_max_volume = 2, set_mute = 3;
}

// Creates and realizes an engine; returns the SLEngineItf pointer. Guest memory: object at data, itf at data+4.
std::uint32_t make_engine(Fixture& f) {
    f.put32(data + 0x10, 1);  // SLEngineOption {SL_ENGINEOPTION_THREADSAFE, SL_BOOLEAN_TRUE}
    f.put32(data + 0x14, 1);
    auto r = f.call(create_engine, {data, 1, data + 0x10, 0, 0, 0});
    check(r.stop == atlan::Stop::returned && f.result() == 0, "slCreateEngine succeeds");
    const auto object = f.get32(data);
    r = f.method(object, slot::realize, {0});
    check(r.stop == atlan::Stop::returned && f.result() == 0, "engine Realize succeeds");
    r = f.method(object, slot::get_interface, {f.iid("SL_IID_ENGINE"), data + 4});
    check(r.stop == atlan::Stop::returned && f.result() == 0, "GetInterface(ENGINE) succeeds");
    return f.get32(data + 4);
}
std::uint32_t make_mix(Fixture& f, std::uint32_t engine) {
    f.put32(data + 0x20, f.iid("SL_IID_ENVIRONMENTALREVERB"));
    f.put32(data + 0x24, 0);  // not required
    const auto r = f.method(engine, slot::create_output_mix, {data + 0x30, 1, data + 0x20, data + 0x24});
    check(r.stop == atlan::Stop::returned && f.result() == 0, "CreateOutputMix with an optional reverb succeeds");
    const auto object = f.get32(data + 0x30);
    check(f.method(object, slot::realize, {0}).stop == atlan::Stop::returned && f.result() == 0, "mix Realize succeeds");
    return object;
}
// Player with an Android simple buffer queue, `buffers` slots, `channels`/`rate`/`bits` PCM and a required volume interface.
std::uint32_t make_player(Fixture& f, std::uint32_t engine, std::uint32_t mix, std::uint32_t buffers, std::uint32_t channels,
                          std::uint32_t rate_hz, std::uint32_t bits, atlan::CallOutcome* outcome = nullptr) {
    f.put32(data + 0x100, 0x800007bd);  // SLDataLocator_AndroidSimpleBufferQueue
    f.put32(data + 0x104, buffers);
    f.put32(data + 0x110, 2);           // SLDataFormat_PCM
    f.put32(data + 0x114, channels);
    f.put32(data + 0x118, rate_hz * 1000);
    f.put32(data + 0x11c, bits);
    f.put32(data + 0x120, bits);
    f.put32(data + 0x124, channels == 1 ? 4 : 3);
    f.put32(data + 0x128, 2);           // little endian
    f.put32(data + 0x130, data + 0x100);  // SLDataSource
    f.put32(data + 0x134, data + 0x110);
    f.put32(data + 0x140, 4);           // SLDataLocator_OutputMix (SL_DATALOCATOR_OUTPUTMIX = 4)
    f.put32(data + 0x144, mix);
    f.put32(data + 0x150, data + 0x140);  // SLDataSink
    f.put32(data + 0x154, 0);
    f.put32(data + 0x160, f.iid("SL_IID_ANDROIDSIMPLEBUFFERQUEUE"));
    f.put32(data + 0x164, f.iid("SL_IID_VOLUME"));
    f.put32(data + 0x170, 1);
    f.put32(data + 0x174, 1);
    auto r = f.method(engine, slot::create_audio_player, {data + 0x180, data + 0x130, data + 0x150, 2, data + 0x160, data + 0x170});
    if (outcome) *outcome = r;
    return r.stop == atlan::Stop::returned && f.result() == 0 ? f.get32(data + 0x180) : 0;
}
std::uint32_t interface_of(Fixture& f, std::uint32_t object, const char* iid_symbol, std::uint32_t out) {
    const auto r = f.method(object, slot::get_interface, {f.iid(iid_symbol), out});
    check(r.stop == atlan::Stop::returned, "GetInterface returns");
    return f.result() == 0 ? f.get32(out) : 0;
}

void android_configuration() {
    Fixture f;
    const auto engine = make_engine(f);
    const auto mix = make_mix(f, engine);
    // Same player as make_player, plus a required SL_IID_ANDROIDCONFIGURATION.
    f.put32(data + 0x100, 0x800007bd); f.put32(data + 0x104, 2);
    f.put32(data + 0x110, 2); f.put32(data + 0x114, 1); f.put32(data + 0x118, 22050000); f.put32(data + 0x11c, 16);
    f.put32(data + 0x120, 16); f.put32(data + 0x124, 4); f.put32(data + 0x128, 2);
    f.put32(data + 0x130, data + 0x100); f.put32(data + 0x134, data + 0x110);
    f.put32(data + 0x140, 4); f.put32(data + 0x144, mix);
    f.put32(data + 0x150, data + 0x140); f.put32(data + 0x154, 0);
    f.put32(data + 0x160, f.iid("SL_IID_ANDROIDSIMPLEBUFFERQUEUE"));
    f.put32(data + 0x164, f.iid("SL_IID_ANDROIDCONFIGURATION"));
    f.put32(data + 0x170, 1); f.put32(data + 0x174, 1);
    auto r = f.method(engine, slot::create_audio_player, {data + 0x180, data + 0x130, data + 0x150, 2, data + 0x160, data + 0x170});
    check(r.stop == atlan::Stop::returned && f.result() == 0, "a player with a required AndroidConfiguration is created");
    const auto player = f.get32(data + 0x180);
    const auto cfg = interface_of(f, player, "SL_IID_ANDROIDCONFIGURATION", data + 0x190);
    check(cfg != 0, "AndroidConfiguration interface is exposed");
    std::vector<std::byte> key(26, std::byte{0});
    std::memcpy(key.data(), "androidPlaybackStreamType", 25);
    check(f.runtime.memory.write(data + 0x200, key), "write key");
    f.put32(data + 0x240, 3);  // SL_ANDROID_STREAM_MEDIA
    r = f.method(cfg, 0, {data + 0x200, data + 0x240, 4});
    check(r.stop == atlan::Stop::returned && f.result() == 0, "SetConfiguration(stream type) succeeds before Realize");
    f.put32(data + 0x250, 4);
    r = f.method(cfg, 1, {data + 0x200, data + 0x250, data + 0x260});
    check(r.stop == atlan::Stop::returned && f.result() == 0 && f.get32(data + 0x250) == 4 && f.get32(data + 0x260) == 3, "GetConfiguration returns the stored value");
    r = f.method(cfg, 0, {data + 0x270, data + 0x240, 4});
    check(r.stop == atlan::Stop::returned && f.result() == 2, "an unknown key is SL_RESULT_PARAMETER_INVALID");
    r = f.method(player, slot::realize, {0});
    check(r.stop == atlan::Stop::returned && f.result() == 0, "player Realize");
    r = f.method(cfg, 0, {data + 0x200, data + 0x240, 4});
    check(r.stop == atlan::Stop::returned && f.result() == 3, "SetConfiguration after Realize is PRECONDITIONS_VIOLATED");
}

void engine_and_mix() {
    Fixture f;
    // Interface IDs are distinct pointers into read-only guest data.
    check(f.iid("SL_IID_PLAY") != f.iid("SL_IID_VOLUME") && f.iid("SL_IID_ENGINE") != 0, "IID pointers are distinct");
    check(!OpenSles::data_symbol("SL_IID_NOT_A_THING") && !OpenSles::data_symbol("slCreateEngine"), "only imported IIDs resolve");
    const auto engine = make_engine(f);
    check(engine != 0, "engine interface pointer is published");
    f.put32(data + 0x20, f.iid("SL_IID_ENVIRONMENTALREVERB"));
    f.put32(data + 0x24, 1);  // required
    auto r = f.method(engine, slot::create_output_mix, {data + 0x30, 1, data + 0x20, data + 0x24});
    check(r.stop == atlan::Stop::returned && f.result() == 12, "a required unsupported interface fails creation with FEATURE_UNSUPPORTED");
    const auto mix = make_mix(f, engine);
    f.put32(data + 0x40, 0xdead);
    r = f.method(mix, slot::get_interface, {f.iid("SL_IID_ENVIRONMENTALREVERB"), data + 0x40});
    check(r.stop == atlan::Stop::returned && f.result() == 12 && f.get32(data + 0x40) == 0, "an optional interface that was not exposed is FEATURE_UNSUPPORTED with a null pointer");
    r = f.method(mix, slot::get_state, {data + 0x44});
    check(f.result() == 0 && f.get32(data + 0x44) == 2, "realized object reports SL_OBJECT_STATE_REALIZED");
    // Failures are explicit stops.
    Fixture g;
    g.put32(data + 0x10, 2);
    g.put32(data + 0x14, 1);
    r = g.call(create_engine, {data, 1, data + 0x10, 0, 0, 0});
    check(r.stop == atlan::Stop::service_error, "an unknown engine option stops");
    r = g.call(create_engine, {0, 0, 0, 0, 0, 0});
    check(r.stop == atlan::Stop::returned && g.result() == 2, "null out pointer is SL_RESULT_PARAMETER_INVALID");
    const auto e2 = make_engine(g);
    r = g.method(e2, slot::create_midi, {0, 0, 0, 0, 0, 0});
    check(r.stop == atlan::Stop::unresolved_import && r.import == "SLEngineItf::CreateMidiPlayer", "an unimplemented method stops by name");
    r = g.method(e2, 4, {});
    check(g.get32(data) != 0, "engine object remains");
}

void player_playback() {
    Fixture f;
    const auto engine = make_engine(f);
    const auto mix = make_mix(f, engine);
    const auto player = make_player(f, engine, mix, 2, 2, 16000, 16);
    check(player != 0, "player is created");
    check(f.sink.stream_count() == 0, "no stream before Realize");
    check(f.method(player, slot::realize, {0}).stop == atlan::Stop::returned && f.result() == 0, "player Realize succeeds");
    check(f.sink.stream_count() == 1 && f.sink.record(0).format == atlan::PcmFormat{16000, 2}, "Realize opens a 16 kHz stereo stream");
    const auto play = interface_of(f, player, "SL_IID_PLAY", data + 0x200);
    const auto queue = interface_of(f, player, "SL_IID_ANDROIDSIMPLEBUFFERQUEUE", data + 0x204);
    const auto volume = interface_of(f, player, "SL_IID_VOLUME", data + 0x208);
    check(play && queue && volume, "play, queue and volume interfaces are exposed");
    check(interface_of(f, player, "SL_IID_BUFFERQUEUE", data + 0x20c) == 0 && f.result() == 12, "the other queue kind is not exposed");
    check(interface_of(f, player, "SL_IID_PLAYBACKRATE", data + 0x20c) == 0 && f.result() == 12, "playback rate was not requested");
    // Completion callback with a guest counter at data+0x300.
    f.put32(data + 0x300, 0);
    check(f.method(queue, slot::register_callback, {code, data + 0x300}).stop == atlan::Stop::returned && f.result() == 0, "RegisterCallback");
    // PCM: 4 frames (16 bytes) then 2 frames (8 bytes).
    for (unsigned i = 0; i < 6; ++i) f.put32(data + 0x400 + 4 * i, 0x11110000 + i);
    check(f.method(play, slot::set_play_state, {3}).stop == atlan::Stop::returned && f.result() == 0, "SetPlayState(playing)");
    check(f.sink.record(0).playing, "the sink stream is playing");
    check(f.method(queue, slot::enqueue, {data + 0x400, 16}).stop == atlan::Stop::returned && f.result() == 0, "Enqueue #1");
    check(f.method(queue, slot::enqueue, {data + 0x410, 8}).stop == atlan::Stop::returned && f.result() == 0, "Enqueue #2");
    check(f.method(queue, slot::enqueue, {data + 0x410, 8}).stop == atlan::Stop::returned && f.result() == 7, "third Enqueue is BUFFER_INSUFFICIENT with a 2-buffer queue");
    check(f.method(queue, slot::enqueue, {data + 0x410, 6}).stop == atlan::Stop::returned && f.result() == 2, "a partial frame is PARAMETER_INVALID");
    check(f.sink.record(0).appended == 24, "the sink received exactly the accepted PCM");
    check(std::memcmp(f.sink.record(0).data.data() + 4, "\x01\x00\x11\x11", 4) == 0, "PCM bytes are copied unmodified");
    check(f.method(queue, slot::queue_state, {data + 0x500}).stop == atlan::Stop::returned && f.get32(data + 0x500) == 2 && f.get32(data + 0x504) == 0,
          "GetState: 2 queued, play index 0");
    auto pump = f.sles->pump(f.runtime, stack, sentinel, 10000);
    check(pump.callbacks == 0 && !pump.error && f.get32(data + 0x300) == 0, "nothing played yet, no callback");
    check(f.sink.consume(0, 16) == 16, "device plays the first buffer");
    pump = f.sles->pump(f.runtime, stack, sentinel, 10000);
    check(pump.callbacks == 1 && !pump.error && f.get32(data + 0x300) == 1 && f.get32(data + 0x304) == queue,
          "one callback, called with the queue interface and context");
    check(f.method(queue, slot::queue_state, {data + 0x500}).stop == atlan::Stop::returned && f.get32(data + 0x500) == 1 && f.get32(data + 0x504) == 1,
          "GetState: 1 queued, play index 1");
    check(f.sink.consume(0, 100) == 8, "device plays the rest");
    pump = f.sles->pump(f.runtime, stack, sentinel, 10000);
    check(pump.callbacks == 1 && f.get32(data + 0x300) == 2, "second callback");
    // Pause holds data; Clear drops it without callbacks.
    check(f.method(play, slot::set_play_state, {2}).stop == atlan::Stop::returned && f.result() == 0 && !f.sink.record(0).playing, "pause stops rendering");
    check(f.method(queue, slot::enqueue, {data + 0x400, 16}).stop == atlan::Stop::returned && f.result() == 0, "enqueue while paused");
    check(f.sink.consume(0, 16) == 0, "paused stream does not consume");
    check(f.method(queue, slot::clear, {}).stop == atlan::Stop::returned && f.result() == 0 && f.sink.record(0).flushes == 1, "Clear flushes the sink");
    pump = f.sles->pump(f.runtime, stack, sentinel, 10000);
    check(pump.callbacks == 0 && f.get32(data + 0x300) == 2, "Clear delivers no callback");
    check(f.method(queue, slot::queue_state, {data + 0x500}).stop == atlan::Stop::returned && f.get32(data + 0x500) == 0, "queue is empty after Clear");
    check(f.method(play, slot::get_play_state, {data + 0x508}).stop == atlan::Stop::returned && f.get32(data + 0x508) == 2, "GetPlayState reports paused");
    check(f.method(play, slot::set_play_state, {9}).stop == atlan::Stop::returned && f.result() == 2, "invalid play state is PARAMETER_INVALID");
    // Volume: millibel -> linear gain; mute overrides.
    check(f.method(volume, slot::set_volume, {static_cast<std::uint32_t>(-6000) & 0xffff}).stop == atlan::Stop::returned && f.result() == 0, "SetVolumeLevel(-60 dB)");
    check(std::abs(f.sink.record(0).gain - 0.001F) < 1e-5F, "-6000 mB is a gain of 0.001");
    check(f.method(volume, slot::set_volume, {500}).stop == atlan::Stop::returned && f.result() == 2, "above the 0 dB maximum is PARAMETER_INVALID");
    check(f.method(volume, slot::get_max_volume, {data + 0x50c}).stop == atlan::Stop::returned && (f.get32(data + 0x50c) & 0xffff) == 0, "max volume level is 0 mB");
    check(f.method(volume, slot::set_mute, {1}).stop == atlan::Stop::returned && f.sink.record(0).gain == 0.0F, "mute silences the stream");
    check(f.method(volume, slot::set_mute, {0}).stop == atlan::Stop::returned && f.sink.record(0).gain > 0.0F, "unmute restores the gain");
    // Unimplemented methods of implemented interfaces stop by name.
    auto r = f.method(play, slot::get_duration, {data + 0x510});
    check(r.stop == atlan::Stop::unresolved_import && r.import == "SLPlayItf::GetDuration", "unimplemented Play method stops by name");
    // Destroy closes the stream; the dead interface is then refused.
    check(f.sles->live_players() == 1, "one live player");
    check(f.method(player, slot::destroy, {}).stop == atlan::Stop::returned && f.sles->live_players() == 0, "Destroy releases the player");
    check(!f.sink.record(0).open, "Destroy closes the sink stream");
    r = f.method(play, slot::get_play_state, {data + 0x508});
    check(r.stop == atlan::Stop::service_error, "a destroyed interface is refused explicitly");
}

void player_rejections() {
    {
        Fixture f;
        const auto engine = make_engine(f);
        const auto mix = make_mix(f, engine);
        atlan::CallOutcome o;
        check(make_player(f, engine, mix, 2, 2, 16000, 8, &o) == 0 && o.stop == atlan::Stop::service_error, "8-bit PCM is refused explicitly");
        check(make_player(f, engine, mix, 2, 6, 16000, 16, &o) == 0 && o.stop == atlan::Stop::service_error, "6-channel PCM is refused explicitly");
        check(make_player(f, engine, mix, 0, 2, 16000, 16, &o) == 0 && o.stop == atlan::Stop::service_error, "zero buffers are refused");
        const auto mono = make_player(f, engine, mix, 4, 1, 8000, 16);
        check(mono != 0, "mono 8 kHz player is created");
        check(f.method(mono, slot::realize, {0}).stop == atlan::Stop::returned && f.sink.record(0).format == atlan::PcmFormat{8000, 1}, "mono format reaches the sink");
        check(f.method(mono, slot::realize, {1}).stop == atlan::Stop::service_error, "async Realize is refused explicitly");
    }
    {
        Fixture f;
        const auto engine = make_engine(f);
        const auto mix = make_mix(f, engine);
        const auto player = make_player(f, engine, mix, 2, 2, 16000, 16);
        const auto queue_before = f.method(player, slot::get_interface, {f.iid("SL_IID_ANDROIDSIMPLEBUFFERQUEUE"), data + 0x204});
        check(queue_before.stop == atlan::Stop::returned && f.result() == 3, "GetInterface before Realize is PRECONDITIONS_VIOLATED");
        f.sink.fail_next_open("device busy");
        check(f.method(player, slot::realize, {0}).stop == atlan::Stop::returned && f.result() == 5, "a refused stream is SL_RESULT_RESOURCE_ERROR");
        check(f.trace.back().find("device busy") != std::string::npos, "the sink's reason is traced");
    }
    {
        Fixture f(false);
        const auto engine = make_engine(f);
        const auto mix = make_mix(f, engine);
        const auto player = make_player(f, engine, mix, 2, 2, 16000, 16);
        const auto r = f.method(player, slot::realize, {0});
        check(r.stop == atlan::Stop::service_error, "a player without a configured sink stops explicitly");
    }
    {
        // Layout collisions are refused at construction.
        atlan::Runtime runtime;
        atlan::ImportDispatcher dispatcher;
        OpenSles ok(runtime, dispatcher, {});
        bool threw = false;
        atlan::ImportDispatcher other;
        try {
        OpenSles again(runtime, other, {}); } catch (const std::invalid_argument&) { threw = true; }
        check(threw, "overlapping OpenSL ES regions are rejected");
    }
}

void semaphores() {
    atlan::Runtime runtime;
    atlan::ImportDispatcher dispatcher;
    atlan::SemaphoreTable table;
    table.install(dispatcher);
    runtime.memory.map(data, 0x1000, atlan::Permissions::read_write);
    runtime.memory.map(stack - 0x1000, 0x1000, atlan::Permissions::read_write);
    atlan::elf::Image image;
    image.trapped_imports = {{"sem_init", 0xf0000000, 2}, {"sem_post", 0xf0000004, 2}, {"sem_wait", 0xf0000008, 2}, {"sem_destroy", 0xf000000c, 2}};
    dispatcher.attach(image);
    const auto call = [&](std::uint32_t address, std::initializer_list<std::uint32_t> args) {
        const std::vector<std::uint32_t> values(args);
        return dispatcher.call(runtime, address, values, stack, sentinel, 1000);
    };
    auto r = call(0xf0000000, {data, 0, 1});
    check(r.stop == atlan::Stop::returned && runtime.cpu.registers[0] == 0 && table.value(data) == 1, "sem_init stores the count");
    check(call(0xf0000008, {data}).stop == atlan::Stop::returned && table.value(data) == 0, "sem_wait decrements");
    r = call(0xf0000008, {data});
    check(r.stop == atlan::Stop::service_error, "waiting on zero stops as a deadlock");
    check(call(0xf0000004, {data}).stop == atlan::Stop::returned && table.value(data) == 1, "sem_post increments");
    check(call(0xf0000004, {data + 8}).stop == atlan::Stop::service_error, "sem_post on an uninitialized semaphore stops");
    check(call(0xf0000000, {data, 1, 0}).stop == atlan::Stop::service_error, "process-shared semaphores are not implemented");
    check(call(0xf000000c, {data}).stop == atlan::Stop::returned && !table.exists(data), "sem_destroy forgets it");
    check(call(0xf000000c, {data}).stop == atlan::Stop::service_error, "double destroy stops");
}
}
// A WAVE file as an app ships its sounds: chunks before and between fmt and data, padding, extensible headers.
std::vector<std::byte> wav(std::uint16_t tag, std::uint16_t bits, std::uint16_t channels, std::size_t data_bytes,
                           std::size_t declared_extra = 0, bool extensible = false) {
    std::vector<std::byte> out;
    const auto bytes = [&](const char* text) { for (unsigned i = 0; i < 4; ++i) out.push_back(std::byte(text[i])); };
    const auto u16 = [&](std::uint32_t v) { out.push_back(std::byte(v & 255)); out.push_back(std::byte((v >> 8) & 255)); };
    const auto u32 = [&](std::uint32_t v) { u16(v & 0xffff); u16(v >> 16); };
    bytes("RIFF"); u32(0); bytes("WAVE");
    bytes("LIST"); u32(3); out.push_back(std::byte{1}); out.push_back(std::byte{2}); out.push_back(std::byte{3});
    out.push_back(std::byte{0});  // pad byte of the odd-sized chunk
    bytes("fmt "); u32(extensible ? 40 : 16);
    u16(extensible ? 0xfffe : tag); u16(channels); u32(22050); u32(22050U * channels * bits / 8); u16(channels * bits / 8); u16(bits);
    if (extensible) { u16(22); u16(bits); u32(3); u16(tag); for (int i = 0; i < 14; ++i) out.push_back(std::byte{0}); }
    bytes("data"); u32(static_cast<std::uint32_t>(data_bytes + declared_extra));
    for (std::size_t i = 0; i < data_bytes; ++i) out.push_back(std::byte(i & 255));
    return out;
}
void wav_files() {
    std::string error;
    auto bytes = wav(1, 16, 2, 400);
    auto pcm = atlan::parse_wav(bytes, error);
    check(pcm && pcm->format == atlan::PcmFormat{22050, 2} && pcm->samples.size() == 400 &&
          pcm->samples.data() == bytes.data() + (bytes.size() - 400), "16-bit PCM after a padded chunk");
    bytes = wav(1, 16, 1, 400, 0, true);
    pcm = atlan::parse_wav(bytes, error);
    check(pcm && pcm->format == atlan::PcmFormat{22050, 1} && pcm->samples.size() == 400, "WAVE_FORMAT_EXTENSIBLE PCM");
    bytes = wav(1, 16, 2, 402, 1000);
    pcm = atlan::parse_wav(bytes, error);
    check(pcm && pcm->samples.size() == 400, "a truncated data chunk is cut to whole frames");
    check(!atlan::parse_wav(wav(1, 8, 1, 10), error) && error.find("8-bit") != std::string::npos, "8-bit samples are refused");
    check(!atlan::parse_wav(wav(3, 32, 1, 10), error), "float samples are refused");
    bytes = wav(1, 16, 1, 10);
    bytes[8] = std::byte{'X'};
    check(!atlan::parse_wav(bytes, error), "other RIFF files are refused");
    check(!atlan::parse_wav(std::span<const std::byte>(bytes.data(), 6), error), "short files are refused");

    const auto path = std::filesystem::temp_directory_path() / ("atlan-wav-" + std::to_string(::getpid()) + ".wav");
    bytes = wav(1, 16, 2, 800);
    std::ofstream(path, std::ios::binary).write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
    atlan::RecordingAudioSink sink;
    auto stream = atlan::play_wav_file(sink, path, error);
    check(stream && sink.stream_count() == 1, "a WAVE file opens one stream");
    auto record = sink.record(0);
    check(record.format == atlan::PcmFormat{22050, 2} && record.data.size() == 800 && record.playing &&
          record.data[1] == std::byte{1}, "the stream plays the file's samples from the start");
    stream.reset();
    check(!sink.record(0).open, "dropping the stream stops the sound");
    sink.fail_next_open("busy");
    check(!atlan::play_wav_file(sink, path, error) && error == "busy", "a refused stream is reported");
    std::filesystem::remove(path);
    check(!atlan::play_wav_file(sink, path, error) && error.find("cannot open") == 0, "a missing file is reported");
}
int main() {
    try {
        wav_files();
        engine_and_mix();
        android_configuration();
        player_playback();
        player_rejections();
        semaphores();
        std::cout << "opensles checks passed: " << checks << '\n';
    } catch (const std::exception& error) {
        std::cerr << "FAILED after " << checks << " checks: " << error.what() << '\n';
        return 1;
    }
}
