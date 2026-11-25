// SPDX-License-Identifier: GPL-2.0-or-later
// atlan-session LIBRARY DATA_ROOT [GL_TRACE_FILE] — runs the guest session headless with a GL call recorder.
#include "atlan/gl_context.hpp"
#include "atlan/gl_recorder.hpp"
#include "shell/session.hpp"
#include <cstdio>
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
    if (const char* frames = std::getenv("ATLAN_FRAMES")) config.render_frames = static_cast<std::uint32_t>(std::atoi(frames));
    atlan::shell::GuestSession session(config, [](const std::string& line) { std::cout << line << std::endl; });
    session.start();
    session.join();
    if (trace) std::fclose(trace);
    const auto& st = context.stats();
    std::cout << "context frames=" << st.frames << " draws=" << st.draws << " dropped=" << st.draws_dropped << " clears=" << st.clears
              << " programs=" << st.programs_linked << " link_failures=" << st.link_failures << " shader_failures=" << st.shader_failures
              << " uploads=" << st.texture_uploads << " unsupported=" << st.unsupported << '\n';
    for (const auto& [name, n] : recorder.counts) std::cout << "gl " << name << " " << n << '\n';
    return session.state() == atlan::shell::SessionState::finished ? 0 : 1;
}
