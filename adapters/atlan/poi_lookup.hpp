// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include "atlan/native_functions.hpp"
#include <vector>

// The map's POI style lookups, on the host.
//
// While the map lays out its POI icons it asks its map settings, for every POI of every visible tile (thousands per
// frame), how that POI category is drawn: one call fills a style record, another gives the icon's symbol data.
// Both only read the settings tables: the category's entry, its list of per-mode variants (the first one for the
// current draw mode, else the default variant), and for symbols a symbol table. They run here as host code, compared
// with the guest's own code on generated tables (in and out of range, every mode, missing variants) before they are
// installed.
//
// Located through the embedder's signature file under the member names "poi_style" and "poi_symbol".
namespace atlan::shell::poi {
[[nodiscard]] std::vector<NativeSet> poi_lookup_sets();
}
