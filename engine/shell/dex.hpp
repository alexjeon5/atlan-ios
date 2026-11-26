// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include <cstddef>
#include <span>
#include <string>
#include <vector>

// The Java methods a Dalvik executable (classes.dex) names, so a native method bound by name can be checked against
// the signature the app declares for it. Exports carry only the name (Java_<class>_<name>), so two builds of an app
// may export the same symbol with different parameters.
namespace atlan::shell {
struct JavaMethod {
    std::string class_name;  // slash form, e.g. example/Input
    std::string name;
    std::string signature;   // JNI form, e.g. (IJFF)V
};

// Every entry of the method_ids table: methods the file declares and those it calls. Throws std::runtime_error when
// the bytes are no dex file or a table points outside it.
[[nodiscard]] std::vector<JavaMethod> dex_methods(std::span<const std::byte> dex);
}
