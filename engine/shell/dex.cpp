// SPDX-License-Identifier: GPL-2.0-or-later
#include "shell/dex.hpp"
#include <cstdint>
#include <stdexcept>

namespace atlan::shell {
namespace {
[[noreturn]] void fail(const std::string& why) { throw std::runtime_error("dex: " + why); }

class Reader {
public:
    explicit Reader(std::span<const std::byte> bytes) : bytes_(bytes) {}
    std::uint32_t u16(std::size_t at) const { need(at, 2); return byte(at) | byte(at + 1) << 8; }
    std::uint32_t u32(std::size_t at) const { need(at, 4); return u16(at) | u16(at + 2) << 16; }
    // string_data_item: ULEB128 UTF-16 length, then MUTF-8 bytes up to a NUL. Type descriptors and member names are
    // ASCII in practice; other bytes are kept as they are.
    std::string string(std::uint32_t index) const {
        if (index >= string_count) fail("string index out of range");
        std::size_t at = u32(string_ids + std::size_t{index} * 4);
        do need(at, 1); while (byte(at++) & 0x80);
        std::string out;
        for (;; ++at) {
            need(at, 1);
            if (byte(at) == 0) return out;
            out.push_back(static_cast<char>(byte(at)));
        }
    }
    std::string type(std::uint32_t index) const {
        if (index >= type_count) fail("type index out of range");
        return string(u32(type_ids + std::size_t{index} * 4));
    }
    std::uint32_t string_count{}, string_ids{}, type_count{}, type_ids{};
private:
    std::uint32_t byte(std::size_t at) const { return std::to_integer<std::uint32_t>(bytes_[at]); }
    void need(std::size_t at, std::size_t n) const { if (at > bytes_.size() || bytes_.size() - at < n) fail("table outside the file"); }
    std::span<const std::byte> bytes_;
};
}

std::vector<JavaMethod> dex_methods(std::span<const std::byte> dex) {
    static constexpr char magic[] = "dex\n";
    if (dex.size() < 0x70) fail("too short");
    for (int i = 0; i < 4; ++i) if (std::to_integer<char>(dex[static_cast<std::size_t>(i)]) != magic[i]) fail("no dex magic");
    Reader r(dex);
    r.string_count = r.u32(0x38); r.string_ids = r.u32(0x3c);
    r.type_count = r.u32(0x40); r.type_ids = r.u32(0x44);
    const std::uint32_t proto_count = r.u32(0x48), proto_ids = r.u32(0x4c);
    const std::uint32_t method_count = r.u32(0x58), method_ids = r.u32(0x5c);
    // A class descriptor Lexample/Input; becomes example/Input, as JNI and the bindings name classes.
    const auto class_name = [](std::string d) { return d.size() > 2 && d.front() == 'L' && d.back() == ';' ? d.substr(1, d.size() - 2) : d; };
    std::vector<std::string> protos;
    protos.reserve(proto_count);
    for (std::uint32_t i = 0; i < proto_count; ++i) {
        const std::size_t at = proto_ids + std::size_t{i} * 12;  // shorty_idx, return_type_idx, parameters_off
        std::string signature = "(";
        if (const std::uint32_t list = r.u32(at + 8); list != 0)
            for (std::uint32_t p = 0, n = r.u32(list); p < n; ++p) signature += r.type(r.u16(list + 4 + std::size_t{p} * 2));
        protos.push_back(signature + ")" + r.type(r.u32(at + 4)));
    }
    std::vector<JavaMethod> out;
    out.reserve(method_count);
    for (std::uint32_t i = 0; i < method_count; ++i) {
        const std::size_t at = method_ids + std::size_t{i} * 8;  // class_idx (u16), proto_idx (u16), name_idx (u32)
        const std::uint32_t proto = r.u16(at + 2);
        if (proto >= protos.size()) fail("proto index out of range");
        out.push_back({class_name(r.type(r.u16(at))), r.string(r.u32(at + 4)), protos[proto]});
    }
    return out;
}
}
