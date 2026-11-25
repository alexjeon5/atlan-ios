// SPDX-License-Identifier: GPL-2.0-or-later
// atlan-session LIBRARY DATA_ROOT [GL_TRACE_FILE] — runs the guest session headless with a GL call recorder.
#include "atlan/gl_context.hpp"
#include "atlan/gl_recorder.hpp"
#include "shell/session.hpp"
#include <chrono>
#include <cstdio>
#include <thread>
#include <cstdlib>
#include <iostream>

int main(int argc, char** argv) {
    if (argc < 3) { std::cerr << "usage: atlan-session LIBRARY DATA_ROOT [GL_TRACE_FILE [--sources]]\n"; return 2; }
    std::FILE* trace = argc > 3 ? std::fopen(argv[3], "w") : nullptr;
    atlan::RecordingGlBackend recorder(trace ? trace : stderr, argc > 4);
    atlan::LoggingGlSink sink(trace ? trace : stderr);
    atlan::GlContext context(sink);
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
    // ATLAN_SURFACE=WIDTHxHEIGHT: host surface size in pixels; the guest screen is sized to fill it (fill_surface).
    atlan::shell::SurfaceState surface;
    unsigned surface_width = 0, surface_height = 0;
    if (const char* size = std::getenv("ATLAN_SURFACE"); size && std::sscanf(size, "%ux%u", &surface_width, &surface_height) == 2) {
        surface.set({surface_width, surface_height});
        config.surface = &surface;
        config.fill_surface = true;
    }
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
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(std::atoi(interactive));
        while (session.state() == atlan::shell::SessionState::running && std::chrono::steady_clock::now() < deadline)
            std::this_thread::sleep_for(std::chrono::milliseconds(50));
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
