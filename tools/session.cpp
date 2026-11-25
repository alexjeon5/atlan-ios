// SPDX-License-Identifier: GPL-2.0-or-later
// atlan-session LIBRARY DATA_ROOT [GL_TRACE_FILE] — runs the guest session headless with a GL call recorder.
// LIBRARY is an ARM32 .so, an APK or an encoded atlan.dat; "" runs the package found in DATA_ROOT/ATLAN.
#include "atlan/gl_context.hpp"
#include "atlan/gl_recorder.hpp"
#include "shell/session.hpp"
#include "preview_sink.hpp"
#include <chrono>
#include <cstdio>
#include <thread>
#include <vector>
#include <cstdlib>
#include <iostream>

int main(int argc, char** argv) {
    if (argc < 3) { std::cerr << "usage: atlan-session LIBRARY DATA_ROOT [GL_TRACE_FILE [--sources]]\n"; return 2; }
    std::FILE* trace = argc > 3 ? std::fopen(argv[3], "w") : nullptr;
    atlan::RecordingGlBackend recorder(trace ? trace : stderr, argc > 4);
    atlan::LoggingGlSink sink(trace ? trace : stderr);
    // ATLAN_PREVIEW=DIR: also write a rough software picture of every ATLAN_PREVIEW_EVERY-th frame (default 1) as
    // DIR/frame_NNNNN.ppm, ATLAN_PREVIEW_SIZE=WxH pixels (default 800x480). See preview_sink.hpp for what it shows.
    unsigned preview_width = 800, preview_height = 480;
    if (const char* size = std::getenv("ATLAN_PREVIEW_SIZE")) std::sscanf(size, "%ux%u", &preview_width, &preview_height);
    const char* preview_directory = std::getenv("ATLAN_PREVIEW");
    const char* preview_every = std::getenv("ATLAN_PREVIEW_EVERY");
    atlan::tools::PreviewGlSink preview(preview_width, preview_height, preview_directory ? preview_directory : "",
                                        preview_every ? static_cast<std::uint32_t>(std::atoi(preview_every)) : 1, &sink);
    atlan::GlContext context(preview_directory ? static_cast<atlan::GlSink&>(preview) : static_cast<atlan::GlSink&>(sink));
    context.set_log([](const std::string& line) { std::cout << "gl: " << line << std::endl; });
    const bool raw = std::getenv("ATLAN_RAW_GL") != nullptr;
    atlan::shell::SessionConfig config;
    config.library_path = argv[1];
    config.data_root = argv[2];
    if (raw) config.gl_backend = &recorder;
    else config.gl_backend = &context;
    config.block_cache = std::getenv("ATLAN_NO_BLOCK_CACHE") == nullptr;
    config.perf_log = std::getenv("ATLAN_PERF") != nullptr;
    if (const char* frames = std::getenv("ATLAN_FRAMES")) config.render_frames = static_cast<std::uint32_t>(std::atoi(frames));
    // ATLAN_TAP=X,Y: where the test tap lands in guest screen pixels (default 400,240).
    float tap_x = 400, tap_y = 240;
    if (const char* tap = std::getenv("ATLAN_TAP")) std::sscanf(tap, "%f,%f", &tap_x, &tap_y);
    // ATLAN_INPUT_CFG: input bindings file (shell/input.hpp). With it, one location fix and one tap are queued before the first frame, to exercise the bound native methods headless.
    atlan::shell::InputQueue input;
    if (const char* path = std::getenv("ATLAN_INPUT_CFG")) {
        std::string why;
        auto bindings = atlan::shell::load_input_bindings(path, why);
        if (!bindings) { std::cerr << "input bindings: " << why << '\n'; return 2; }
        config.input_bindings = std::move(*bindings);
        config.input = &input;
        input.push(atlan::shell::LocationFix{37.5665, 126.9780, 38.0, 0.0, 0.0, 5.0, 1700000000000ULL});
        input.push(atlan::shell::TouchEvent{atlan::shell::TouchPhase::down, 0, tap_x, tap_y, 1000});
        input.push(atlan::shell::TouchEvent{atlan::shell::TouchPhase::up, 0, tap_x, tap_y, 1050});
    }
    // ATLAN_INTERACTIVE_SECONDS=N: keep drawing after the first frames for N seconds (the interactive loop);
    // ATLAN_ON_DEMAND=1 draws those frames only when the guest asks for one.
    const char* interactive = std::getenv("ATLAN_INTERACTIVE_SECONDS");
    config.interactive = interactive != nullptr;
    config.render_on_demand = std::getenv("ATLAN_ON_DEMAND") != nullptr;
    if (config.interactive && !config.input) config.input = &input;
    atlan::shell::GuestSession session(config, [](const std::string& line) { std::cout << line << std::endl; });
    session.start();
    if (interactive) {
        // ATLAN_TAPS="MS:X,Y;MS:X,Y;...": taps (down, up 80 ms later) at those times after the start, in guest pixels.
        struct Tap { long ms; float x, y; };
        std::vector<Tap> taps;
        if (const char* list = std::getenv("ATLAN_TAPS")) {
            Tap tap{};
            int used = 0;
            while (std::sscanf(list, "%ld:%f,%f%n", &tap.ms, &tap.x, &tap.y, &used) == 3) {
                taps.push_back(tap);
                list += used;
                if (*list == ';') ++list;
            }
        }
        const auto start = std::chrono::steady_clock::now();
        const auto deadline = start + std::chrono::seconds(std::atoi(interactive));
        std::size_t next_tap = 0;
        while (session.state() == atlan::shell::SessionState::running && std::chrono::steady_clock::now() < deadline) {
            const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - start).count();
            if (next_tap < taps.size() && elapsed >= taps[next_tap].ms) {
                const auto& tap = taps[next_tap++];
                input.push(atlan::shell::TouchEvent{atlan::shell::TouchPhase::down, 0, tap.x, tap.y, static_cast<std::uint64_t>(elapsed)});
                std::this_thread::sleep_for(std::chrono::milliseconds(80));
                input.push(atlan::shell::TouchEvent{atlan::shell::TouchPhase::up, 0, tap.x, tap.y, static_cast<std::uint64_t>(elapsed + 80)});
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(20));
        }
        session.stop();
    }
    session.join();
    if (trace) std::fclose(trace);
    const auto& st = context.stats();
    std::cout << "context frames=" << st.frames << " draws=" << st.draws << " dropped=" << st.draws_dropped << " clears=" << st.clears
              << " programs=" << st.programs_linked << " link_failures=" << st.link_failures << " shader_failures=" << st.shader_failures
              << " uploads=" << st.texture_uploads << " unsupported=" << st.unsupported << '\n';
    for (const auto& [name, n] : recorder.counts) std::cout << "gl " << name << " " << n << '\n';
    return session.state() == atlan::shell::SessionState::finished ? 0 : 1;
}
