// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include "atlan/imports.hpp"
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace atlan {
// The subset of OpenSL ES the guest actually uses. Objects and interfaces are
// words in guest memory pointing at read-only vtables whose slots are distinct
// unmapped trap addresses: a method the host has not implemented stops by name
// ("SLEngineItf::CreateAudioPlayer") instead of returning a made-up result.
// Interface IDs (SL_IID_*) are data imports resolved with data_symbol().
class OpenSles {
public:
    struct Config {
        std::uint32_t data_base{0xf5000000};    // read-only: IID words, GUIDs, vtables
        std::uint32_t trap_base{0xf6000000};    // unmapped method trap range
        std::uint32_t object_base{0xf5100000};  // read-write: object/interface words
        std::uint32_t object_pages{16};
        std::function<void(const std::string&)> trace;  // optional: one line per observed call
    };
    static constexpr std::uint32_t default_data_base = 0xf5000000;
    // Address of the guest word holding the SLInterfaceID pointer for an imported
    // SL_IID_* data symbol, or nullopt if the name is not an interface ID.
    [[nodiscard]] static std::optional<std::uint32_t> data_symbol(std::string_view name,
                                                                  std::uint32_t data_base = default_data_base);
    // Maps the tables into runtime.memory and binds the implemented services.
    // Throws std::invalid_argument if the regions overlap existing mappings.
    OpenSles(Runtime& runtime, ImportDispatcher& dispatcher, Config config);
    OpenSles(const OpenSles&) = delete;
    OpenSles& operator=(const OpenSles&) = delete;
    ~OpenSles();
private:
    struct State;
    std::shared_ptr<State> state_;
};
}
