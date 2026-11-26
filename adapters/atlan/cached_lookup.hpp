// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include "atlan/native_functions.hpp"
#include "atlan/stdio.hpp"
#include <cstdint>
#include <memory>
#include <vector>

// The current map mode, remembered between file changes.
//
// The app asks for its map mode many times per frame. With the map choice on (adapter: STARTMODEUSE 0) the answer
// comes from its settings lookup, which reads and parses a settings file on every call; on the car units the
// launcher answered from memory instead. The getter is a thin wrapper: it passes an entry number and the address of
// a stack word to the lookup and returns that word. The host checks this shape in the located code, runs the lookup
// itself (as guest code) and notes the files it resolved (GuestStdio::record_paths()). The result is kept for as long
// as those files look the same on the host (identity, size, modification time), checked on every call, or until the
// session starts over. Before the app's start-up has finished every call runs the lookup, because the lookup's
// paths are only settled then.
//
// The getter is located through the embedder's signature file under the member name "map_mode".
namespace atlan::shell::cached_lookup {
struct State;
using Shared = std::shared_ptr<State>;
[[nodiscard]] Shared make_state();
// The session's files (set when they are laid out); a new session forgets what was kept.
void attach(const Shared& state, GuestStdio* files);
// The app's start-up has finished: results may be kept from now on.
void arm(const Shared& state);
[[nodiscard]] std::vector<NativeSet> cached_lookup_sets(Shared state);
}
