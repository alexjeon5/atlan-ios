// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include "atlan/imports.hpp"
#include <functional>
#include <map>
#include <string>

namespace atlan {
// C string/memory comparison and search services over guest memory: strncpy,
// strcmp, strncmp, memcmp, strchr, strrchr. Reads are bounded and never touch
// bytes past the terminator or count that C would not read.
void install_string_services(ImportDispatcher& dispatcher);

// Android system properties: int __system_property_get(const char* name, char* value).
// Names missing from `properties` are unset: value is "" and the result 0, which is
// what a device without that property reports. `trace` (optional) sees every lookup.
// `properties` and the trace callback must outlive the dispatcher.
void install_system_properties(ImportDispatcher& dispatcher, const std::map<std::string, std::string>& properties,
                               std::function<void(const std::string&, bool)> trace = {});
}
