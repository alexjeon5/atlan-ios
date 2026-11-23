// SPDX-License-Identifier: GPL-2.0-or-later
#include "atlan/string_services.hpp"
#include "atlan/abi.hpp"
#include <vector>

namespace atlan {
namespace {
constexpr std::uint32_t max_scan = 64U * 1024 * 1024;
bool byte_at(Memory& memory, std::uint32_t address, std::uint32_t offset, std::uint8_t& out) {
    if (std::uint64_t{address} + offset > 0xffffffffULL) return false;
    std::byte value{};
    if (!memory.read(address + offset, std::span<std::byte>(&value, 1))) return false;
    out = std::to_integer<std::uint8_t>(value);
    return true;
}
// Compares up to `limit` bytes, stopping after a NUL when `text` is set.
std::optional<std::string> compare(ImportCall& call, bool text, std::uint32_t limit, const char* name) {
    if (limit > max_scan) return std::string(name) + " size exceeds service bound";
    for (std::uint32_t i = 0; i < limit; ++i) {
        std::uint8_t a{}, b{};
        if (!byte_at(call.runtime().memory, call.arg(0), i, a) || !byte_at(call.runtime().memory, call.arg(1), i, b))
            return std::string(name) + " read left mapped guest memory";
        if (a != b || (text && a == 0)) {
            call.set_result(static_cast<std::uint32_t>(static_cast<int>(a) - static_cast<int>(b)));
            return std::nullopt;
        }
    }
    call.set_result(0);
    return std::nullopt;
}
}
void install_string_services(ImportDispatcher& dispatcher) {
    dispatcher.bind("strcmp", [](ImportCall& call) { return compare(call, true, max_scan, "strcmp"); });
    dispatcher.bind("strncmp", [](ImportCall& call) { return compare(call, true, call.arg(2), "strncmp"); });
    dispatcher.bind("memcmp", [](ImportCall& call) { return compare(call, false, call.arg(2), "memcmp"); });
    // char* strncpy(char* dst, const char* src, size_t n): pads with NUL up to n.
    dispatcher.bind("strncpy", [](ImportCall& call) -> std::optional<std::string> {
        const auto dst = call.arg(0), src = call.arg(1), count = call.arg(2);
        if (count > max_scan) return "strncpy size exceeds service bound";
        if (count != 0 && std::uint64_t{dst} + count > 0x100000000ULL) return "strncpy wraps the guest address space";
        std::vector<std::byte> out(count, std::byte{0});
        bool ended = false;
        for (std::uint32_t i = 0; i < count && !ended; ++i) {
            std::uint8_t value{};
            if (!byte_at(call.runtime().memory, src, i, value)) return "strncpy source left mapped guest memory";
            out[i] = std::byte{value};
            ended = value == 0;
        }
        if (count != 0 && !call.runtime().memory.write(dst, out)) return "strncpy guest memory fault";
        call.set_result(dst);
        return std::nullopt;
    });
    // char* strchr(const char* s, int c) / strrchr: the terminator is part of the string.
    for (const bool last : {false, true}) {
        dispatcher.bind(last ? "strrchr" : "strchr", [last](ImportCall& call) -> std::optional<std::string> {
            const auto start = call.arg(0);
            const auto wanted = static_cast<std::uint8_t>(call.arg(1) & 0xff);
            std::uint32_t found = 0;
            for (std::uint32_t i = 0; i < max_scan; ++i) {
                std::uint8_t value{};
                if (!byte_at(call.runtime().memory, start, i, value)) return "strchr read left mapped guest memory";
                if (value == wanted) {
                    found = start + i;
                    if (!last) break;
                }
                if (value == 0) { call.set_result(found); return std::nullopt; }
            }
            if (!last && found != 0) { call.set_result(found); return std::nullopt; }
            return "strchr exceeds service bound";
        });
    }
}
void install_system_properties(ImportDispatcher& dispatcher, const std::map<std::string, std::string>& properties,
                               std::function<void(const std::string&, bool)> trace) {
    dispatcher.bind("__system_property_get", [&properties, trace = std::move(trace)](ImportCall& call) -> std::optional<std::string> {
        std::string name;
        if (!read_guest_string(call.runtime().memory, call.arg(0), 32, name)) return "__system_property_get name is unreadable or longer than 32 bytes";
        const auto found = properties.find(name);
        if (trace) trace(name, found != properties.end());
        std::string value = found == properties.end() ? std::string() : found->second;
        if (value.size() > 91) return "__system_property_get value exceeds PROP_VALUE_MAX";
        std::vector<std::byte> bytes(value.size() + 1, std::byte{0});
        for (std::size_t i = 0; i < value.size(); ++i) bytes[i] = static_cast<std::byte>(value[i]);
        if (!call.runtime().memory.write(call.arg(1), bytes)) return "__system_property_get value buffer is not writable";
        call.set_result(static_cast<std::uint32_t>(value.size()));
        return std::nullopt;
    });
}
}
