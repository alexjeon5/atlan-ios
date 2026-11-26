// SPDX-License-Identifier: GPL-2.0-or-later
// Synthetic APK and encoded .dat packages: DEFLATE decoding, library selection, .dat decoding and discovery, and the
// launcher's view of a synthetic data folder.
#include "adapters/atlan/app_package.hpp"
#include "adapters/atlan/data_status.hpp"
#include "adapters/atlan/verified_versions.hpp"
#include "shell/json.hpp"
#include "shell/sha256.hpp"
#include <chrono>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iostream>
#include <stdexcept>
#include <string>
#include <unistd.h>
#include <vector>

namespace {
namespace fs = std::filesystem;
using Bytes = std::vector<std::byte>;
unsigned checks{};
void check(bool result, const char* message) {
    ++checks;
    if (!result) throw std::runtime_error(message);
}
template <class F> void throws(F&& body, const std::string& fragment, const char* message) {
    ++checks;
    try { body(); } catch (const std::runtime_error& error) {
        if (std::string(error.what()).find(fragment) != std::string::npos) return;
        throw std::runtime_error(std::string(message) + ": " + error.what());
    }
    throw std::runtime_error(std::string(message) + ": no error");
}
Bytes bytes(std::initializer_list<unsigned> values) {
    Bytes result;
    for (auto v : values) result.push_back(std::byte(v));
    return result;
}
Bytes bytes(const std::string& text) {
    Bytes result;
    for (char c : text) result.push_back(std::byte(static_cast<unsigned char>(c)));
    return result;
}

// zlib (raw, level 9) output for the texts below.
const Bytes fixed_stream = bytes({0x73, 0x0c, 0xf2, 0x35, 0x36, 0x52, 0x48, 0x2f, 0x4d, 0x2d, 0x2e, 0x01, 0x00});
const std::string fixed_text = "ARM32 guest";
const Bytes dynamic_stream = bytes({
    0x9d, 0xd6, 0x59, 0x52, 0x03, 0x31, 0x0c, 0x45, 0xd1, 0x7f, 0x56, 0xe1, 0x25, 0xc4, 0x83, 0x3c,
    0xb0, 0x1b, 0x86, 0x06, 0x02, 0x21, 0x0d, 0x81, 0x30, 0xad, 0x9e, 0x82, 0x7e, 0x5a, 0xc0, 0xf5,
    0x77, 0xea, 0x95, 0x65, 0xeb, 0x48, 0x9d, 0xc3, 0xfe, 0xb8, 0x84, 0xdd, 0x65, 0x78, 0x7f, 0x58,
    0xc2, 0xeb, 0x79, 0x7f, 0xf3, 0x14, 0xae, 0x4f, 0xeb, 0xe7, 0x31, 0xdc, 0xad, 0x5f, 0xe1, 0xf1,
    0xfc, 0xfc, 0xf2, 0x16, 0xd6, 0x8f, 0xe5, 0xf4, 0xff, 0xf3, 0xe1, 0xea, 0xe7, 0x3b, 0xdc, 0xae,
    0xf7, 0x61, 0x77, 0x71, 0xf8, 0x4b, 0x45, 0x96, 0x8a, 0x5b, 0x2a, 0xb1, 0x54, 0xd9, 0x52, 0x99,
    0xa5, 0xc6, 0x96, 0x2a, 0xb0, 0xc2, 0xba, 0xc5, 0x8c, 0xc5, 0x92, 0x6d, 0xb1, 0xca, 0x62, 0x59,
    0xa7, 0x35, 0xf8, 0x20, 0xba, 0x5b, 0x67, 0xb1, 0xaa, 0x87, 0x1c, 0x2c, 0xd6, 0xd5, 0xb5, 0x08,
    0x89, 0x64, 0xc5, 0xa0, 0x91, 0xa4, 0x2a, 0x23, 0x55, 0xd2, 0x94, 0x83, 0x4e, 0x5a, 0x52, 0x0e,
    0x4a, 0xf1, 0x18, 0x94, 0x92, 0xfd, 0x35, 0x21, 0x95, 0xea, 0xe7, 0x41, 0x2b, 0x43, 0x32, 0x23,
    0xc4, 0x92, 0xbd, 0x7d, 0x50, 0x4b, 0xd3, 0x66, 0x48, 0x50, 0x4b, 0xd4, 0xfd, 0x12, 0xe4, 0x62,
    0xaa, 0x33, 0x41, 0x2e, 0x43, 0xa3, 0x97, 0x20, 0x97, 0x22, 0x9e, 0x09, 0x72, 0x19, 0xbe, 0xfb,
    0xa0, 0x97, 0xe2, 0xf7, 0x83, 0x5e, 0x86, 0xd7, 0x09, 0xbd, 0x98, 0xf7, 0x0f, 0x7a, 0xe9, 0x8a,
    0x41, 0x2e, 0x55, 0x3c, 0x33, 0xe4, 0x92, 0x34, 0xed, 0x19, 0x72, 0xe9, 0xaa, 0x33, 0x43, 0x2e,
    0xe6, 0x1f, 0x21, 0xc8, 0x25, 0x89, 0x75, 0x86, 0x5c, 0xba, 0x56, 0x7c, 0x86, 0x5c, 0xaa, 0x98,
    0x65, 0xfa, 0x25, 0xf2, 0x3e, 0x40, 0x2e, 0xd1, 0xcf, 0xa3, 0x5c, 0x34, 0x7e, 0x99, 0x7a, 0x51,
    0xae, 0x40, 0x2f, 0x45, 0x7d, 0x2f, 0xd0, 0x4b, 0x56, 0xff, 0x0a, 0xf4, 0x12, 0xfd, 0x3c, 0xe8,
    0xc5, 0xaf, 0x47, 0xb7, 0x8b, 0xb6, 0x44, 0x81, 0x5c, 0xba, 0xda, 0x5e, 0x20, 0x97, 0xe6, 0xff,
    0xae, 0x20, 0x97, 0xe6, 0xe7, 0x41, 0x2e, 0xcd, 0xef, 0x37, 0xe6, 0x72, 0xb6, 0x9b, 0xab, 0xd3,
    0xe2, 0xdc, 0xbb, 0x58, 0x9a, 0xeb, 0x83, 0xe5, 0xb9, 0xbe, 0x5b, 0x99, 0x62, 0x66, 0x36, 0xa7,
    0xda, 0xea, 0xdc, 0x14, 0x59, 0x9b, 0x9b, 0x5a, 0xeb, 0x73, 0x5b, 0xc2, 0x06, 0xde, 0x4a, 0xbf,
});
std::string dynamic_text() {
    std::string text;
    char line[96];
    for (int i = 0; i < 60; ++i) {
        std::snprintf(line, sizeof line, "line %d: the quick brown fox jumps over the lazy dog %d\n", i, i * i % 97);
        text += line;
    }
    return text;
}

struct Item {
    std::string name;
    Bytes data;            // uncompressed contents
    Bytes stream;          // DEFLATE stream; empty stores `data`
    std::uint32_t crc_xor{};
};
void le(Bytes& out, std::uint32_t value, unsigned size) {
    for (unsigned i = 0; i < size; ++i) out.push_back(std::byte((value >> (8 * i)) & 255));
}
Bytes zip(const std::vector<Item>& items) {
    Bytes out, directory;
    for (const auto& item : items) {
        const Bytes& body = item.stream.empty() ? item.data : item.stream;
        const std::uint32_t method = item.stream.empty() ? 0 : 8, crc = atlan::shell::crc32(item.data) ^ item.crc_xor;
        const auto offset = static_cast<std::uint32_t>(out.size());
        le(out, 0x04034b50, 4); le(out, 20, 2); le(out, 0, 2); le(out, method, 2); le(out, 0, 4);
        le(out, crc, 4); le(out, static_cast<std::uint32_t>(body.size()), 4); le(out, static_cast<std::uint32_t>(item.data.size()), 4);
        le(out, static_cast<std::uint32_t>(item.name.size()), 2); le(out, 0, 2);
        const Bytes name = bytes(item.name);
        out.insert(out.end(), name.begin(), name.end());
        out.insert(out.end(), body.begin(), body.end());
        le(directory, 0x02014b50, 4); le(directory, 20, 2); le(directory, 20, 2); le(directory, 0, 2); le(directory, method, 2);
        le(directory, 0, 4); le(directory, crc, 4); le(directory, static_cast<std::uint32_t>(body.size()), 4);
        le(directory, static_cast<std::uint32_t>(item.data.size()), 4); le(directory, static_cast<std::uint32_t>(item.name.size()), 2);
        le(directory, 0, 2); le(directory, 0, 2); le(directory, 0, 2); le(directory, 0, 2); le(directory, 0, 4); le(directory, offset, 4);
        directory.insert(directory.end(), name.begin(), name.end());
    }
    const auto start = static_cast<std::uint32_t>(out.size());
    out.insert(out.end(), directory.begin(), directory.end());
    le(out, 0x06054b50, 4); le(out, 0, 2); le(out, 0, 2); le(out, static_cast<std::uint32_t>(items.size()), 2);
    le(out, static_cast<std::uint32_t>(items.size()), 2); le(out, static_cast<std::uint32_t>(directory.size()), 4); le(out, start, 4); le(out, 0, 2);
    return out;
}
// The app's atlan.dat layout: XOR key, four version bytes, payload size, then the payload XORed in whole 4-byte groups.
Bytes encode_dat(const Bytes& apk, Bytes key, Bytes version) {
    Bytes out = key;
    out.insert(out.end(), version.begin(), version.end());
    le(out, static_cast<std::uint32_t>(apk.size()), 4);
    for (std::size_t i = 0; i < apk.size(); ++i) out.push_back(i < apk.size() / 4 * 4 ? apk[i] ^ key[i % 4] : apk[i]);
    return out;
}
Bytes elf_like(unsigned tag) {
    Bytes data = bytes({0x7f, 'E', 'L', 'F', 1, 1, 1, 0});
    for (unsigned i = 0; i < 37; ++i) data.push_back(std::byte((tag * 31 + i) & 255));
    return data;
}
void write(const fs::path& path, const Bytes& data) {
    fs::create_directories(path.parent_path());
    std::ofstream(path, std::ios::binary).write(reinterpret_cast<const char*>(data.data()), static_cast<std::streamsize>(data.size()));
}

void inflating() {
    using atlan::shell::inflate_raw;
    check(inflate_raw(fixed_stream, fixed_text.size()) == bytes(fixed_text), "fixed Huffman block decodes");
    const std::string text = dynamic_text();
    check(inflate_raw(dynamic_stream, text.size()) == bytes(text), "dynamic Huffman block with matches decodes");
    // A final stored block: BFINAL=1, BTYPE=00, LEN=3, NLEN=~3.
    check(inflate_raw(bytes({0x01, 0x03, 0x00, 0xfc, 0xff, 'a', 'b', 'c'}), 3) == bytes("abc"), "stored block decodes");
    throws([] { (void)inflate_raw(bytes({0x01, 0x03, 0x00, 0xfc, 0xfe, 'a', 'b', 'c'}), 3); }, "stored length", "stored length check");
    throws([] { (void)inflate_raw(bytes({0x07}), 1); }, "block type 3", "reserved block type is rejected");
    throws([&] { (void)inflate_raw(Bytes(fixed_stream.begin(), fixed_stream.end() - 3), fixed_text.size()); }, "truncated", "truncated stream");
    throws([&] { (void)inflate_raw(fixed_stream, fixed_text.size() - 1); }, "longer", "output past the recorded size");
    throws([&] { (void)inflate_raw(fixed_stream, fixed_text.size() + 1); }, "shorter", "output short of the recorded size");
    check(atlan::shell::crc32(bytes("123456789")) == 0xcbf43926u, "CRC-32 check value");
}

void packages() {
    using atlan::shell::open_package, atlan::shell::is_package;
    const Bytes atlan = elf_like(1), other = elf_like(2);
    const Bytes apk = zip({{"AndroidManifest.xml", bytes("x"), {}},
                           {"assets/version.txt", bytes(fixed_text), fixed_stream},
                           {"lib/armeabi/libother.so", other, {}},
                           {"lib/armeabi/libatlan3dv5.so", atlan, {}},
                           {"lib/x86/libatlan3dv5.so", other, {}}});
    check(is_package(apk), "an APK is a package");
    auto package = open_package(apk);
    check(package.library == atlan && package.entry == "lib/armeabi/libatlan3dv5.so", "the app's own ARM32 library is chosen");
    check(package.version == fixed_text, "the version comes from assets/version.txt");

    const Bytes dat = encode_dat(apk, bytes({0x25, 0x14, 0x20, 0xfd}), bytes({1, 8, 2, 35}));
    check(is_package(dat) && is_package(std::span(dat).first(16), dat.size()), "an encoded .dat is a package from its head");
    check(!is_package(std::span(dat).first(16), dat.size() - 1), "a .dat whose payload does not fit is not a package");
    package = open_package(dat);
    check(package.library == atlan && package.version == fixed_text, "an encoded .dat opens to the same library");

    const Bytes plain = zip({{"lib/armeabi-v7a/libnav.so", other, {}}});
    // Payload sizes that are not a multiple of 4 keep their 1-3 byte tail unencoded.
    for (unsigned pad = 0; pad < 4; ++pad) {
        Bytes padded = plain;
        padded.insert(padded.end(), pad, std::byte{0});  // trailing bytes after the directory end
        const auto opened = open_package(encode_dat(padded, bytes({0xa5, 0x5a, 0x0f, 0xf0}), bytes({1, 2, 3, 4})));
        check(opened.library == other && opened.entry == "lib/armeabi-v7a/libnav.so", "a lone ARM32 library of any name is chosen");
        check(opened.version == "1.2.3.4", "without version.txt the .dat header gives the version");
    }
    check(open_package(zip({{"lib/armeabi/libatlan.so", other, {}}, {"lib/armeabi-v7a/libatlan.so", atlan, {}}})).library == atlan,
          "armeabi-v7a is preferred");

    throws([] { (void)open_package(bytes("not a package at all")); }, "not an APK", "junk is rejected");
    throws([] { (void)open_package(zip({{"classes.dex", bytes("d"), {}}})); }, "no native library", "an APK without a library");
    throws([&] { (void)open_package(zip({{"lib/arm64-v8a/libatlan3dv5.so", atlan, {}}})); }, "no 32-bit ARM library (found lib/arm64-v8a/libatlan3dv5.so)",
           "a 64-bit-only APK names what it found");
    throws([&] { (void)open_package(zip({{"lib/armeabi/liba.so", atlan, {}}, {"lib/armeabi/libb.so", other, {}}})); }, "several",
           "an ambiguous library choice is rejected");
    throws([&] { (void)open_package(zip({{"lib/armeabi/libatlan3dv5.so", atlan, {}, 1}})); }, "CRC", "a damaged entry fails its CRC");
    Bytes cut = apk;
    cut.resize(cut.size() - 30);
    throws([&] { (void)open_package(cut); }, "ZIP", "a truncated APK is rejected");
}

void discovery(const fs::path& scratch) {
    using atlan::shell::find_package;
    const Bytes apk = zip({{"lib/armeabi/libatlan3dv5.so", elf_like(3), {}}});
    const fs::path root = scratch / "Documents";
    check(find_package(root.string()).empty(), "no data root, no package");
    write(root / "ATLAN" / "APP" / "system.ini", bytes("ROOTPATH|2=/x\n"));
    write(root / "ATLAN" / "APP" / "fake.apk", bytes("PK not really"));
    check(find_package(root.string()).empty(), "files that are not packages are skipped");
    const fs::path dat = root / "ATLAN" / "APP" / "atlan.dat";
    write(dat, encode_dat(apk, bytes({1, 2, 3, 4}), bytes({1, 8, 2, 35})));
    check(find_package(root.string()) == dat.string(), "ATLAN/APP/atlan.dat is found");
    const fs::path newer = root / "ATLAN" / "Atlan_8.3.apk";
    write(newer, apk);
    const auto now = fs::file_time_type::clock::now();
    fs::last_write_time(dat, now - std::chrono::hours(1));
    fs::last_write_time(newer, now);
    check(find_package(root.string()) == newer.string(), "the newest package wins");
    fs::last_write_time(newer, now - std::chrono::hours(2));
    check(find_package(root.string()) == dat.string(), "an older APK does not replace atlan.dat");
    const fs::path other_case = scratch / "Lower";
    write(other_case / "atlan" / "app" / "ATLAN.DAT", encode_dat(apk, bytes({9, 9, 9, 9}), bytes({1, 0, 0, 1})));
    check(find_package(other_case.string()) == (other_case / "atlan" / "app" / "ATLAN.DAT").string(), "folder and file names match in any case");
    check(atlan::shell::find_package_in((other_case / "atlan").string()) == (other_case / "atlan" / "app" / "ATLAN.DAT").string(),
          "the search also starts inside a picked ATLAN folder");
}

Bytes trailer(std::string_view date, std::string_view tag = "ATLANVERINFO") {
    Bytes out = bytes("data");
    Bytes tail(48, std::byte{0});
    for (std::size_t i = 0; i < tag.size(); ++i) tail[i] = std::byte(tag[i]);
    for (std::size_t i = 0; i < date.size(); ++i) tail[32 + i] = std::byte(date[i]);
    out.insert(out.end(), tail.begin(), tail.end());
    return out;
}

void data_status(const fs::path& scratch) {
    using namespace atlan::shell;
    const fs::path picked = scratch / "Picked";
    const fs::path atlan = picked / "atlan";
    check(!inspect_atlan_data(atlan.string()).found, "a missing folder is not found");
    fs::create_directories(atlan / "map" / "map");
    fs::create_directories(atlan / "UI");
    auto status = inspect_atlan_data(atlan.string());
    check(status.found && !status.folders_present() && !status.ready(), "a folder without APP and Sys is not ready");
    check(status.folders[2].present && status.folders[1].present && !status.folders[0].present && !status.folders[3].present,
          "each top folder is checked in any case");
    check(resolve_atlan_folder(picked.string()) == atlan.string() && resolve_atlan_folder(atlan.string()) == atlan.string(),
          "a picked parent leads to its ATLAN folder");
    write(atlan / "app" / "Atlan3d.inf", bytes("Atlan3DA (6.4.21)"));
    write(atlan / "app" / "atlan.dat", encode_dat(zip({{"lib/armeabi/libatlan3dv5.so", elf_like(3), {}}}), bytes({1, 2, 3, 4}), bytes({1, 6, 4, 21})));
    fs::create_directories(atlan / "SYS");
    write(atlan / "map" / "map" / "BASE3D.MAP", trailer("261020"));
    write(atlan / "map" / "NDB" / "road.ndb", trailer("261020", "OTHERTAG"));
    write(atlan / "map" / "map" / "camerat1.map", trailer("2611"));
    write(atlan / "map" / "map" / "camerat2.map", bytes("short"));
    status = inspect_atlan_data(atlan.string());
    check(status.ready(), "all four folders and a package make a ready folder");
    check(status.app_name == "Atlan3DA" && status.app_version == "6.4.21" && status.app_label() == "Atlan3DA 6.4.21", "the app's name and version");
    const auto digest = package_library_sha256(atlan.string());
    check(digest == atlan::shell::sha256_hex(elf_like(3)), "the digest is the native library's, not the package's");
    check(package_library_sha256(atlan.string()) == digest, "a package checked again gives the same digest");
    check(status.map == "26.10.20", "a data file's trailer date");
    check(status.route.empty() && status.search.empty() && status.safety_main.empty() && status.safety_sub.empty(),
          "a wrong tag, a missing file, a short date and a short file give no version");
    fs::create_directories(picked / "ATLAN_RTA" / "Sys");
    fs::create_directories(picked / "logs");
    const auto sets = find_atlan_folders(picked.string());
    check(sets.size() == 2 && sets[0] == (picked / "ATLAN_RTA").string() && sets[1] == atlan.string(),
          "several data sets side by side are found by their top folders, other folders are not");
    write(atlan / "app" / "atlan.dat", encode_dat(zip({{"lib/armeabi/libatlan3dv5.so", elf_like(4), {}}}), bytes({1, 2, 3, 4}), bytes({1, 6, 4, 21})));
    fs::last_write_time(atlan / "app" / "atlan.dat", fs::last_write_time(atlan / "app" / "atlan.dat") + std::chrono::seconds(5));
    check(package_library_sha256(atlan.string()) == atlan::shell::sha256_hex(elf_like(4)), "a replaced package is hashed again");
    write(atlan / "app" / "atlan.dat", bytes("not a package at all"));
    check(package_library_sha256(atlan.string()).empty(), "a broken package has no digest");
    check(package_library_sha256((picked / "ATLAN_RTA").string()).empty(), "a folder without a package has no digest");
}

// FIPS 180-4 example vectors, and a long input fed in uneven pieces.
void digests() {
    using atlan::shell::sha256_hex;
    check(sha256_hex({}) == "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855", "empty input");
    check(sha256_hex(bytes(std::string("abc"))) == "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad", "abc");
    check(sha256_hex(bytes(std::string("abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq"))) ==
              "248d6a61d20638b8e5c026930c3e6039a33ce45964ff2167f6ecedd419db06c1", "two blocks");
    const Bytes million(1000000, std::byte{'a'});
    atlan::shell::Sha256 pieces;
    for (std::size_t at = 0; at < million.size();) {
        const auto n = std::min<std::size_t>(million.size() - at, 1 + at % 97);
        pieces.update(std::span(million).subspan(at, n));
        at += n;
    }
    check(pieces.hex() == "cdc76e5c9914fb9281a1c7e284d73e67f1809a48a497200e046d39ccc7112cd0", "a million a's in pieces");
}

void json() {
    using atlan::shell::JsonValue;
    using atlan::shell::parse_json;
    std::string why;
    const auto doc = parse_json(R"( {"a": [1, -2.5e1, true, false, null], "s": "\u00e9\ud83d\ude00\n\"", "o": {}} )", why);
    check(doc && doc->kind == JsonValue::Kind::object, "an object");
    const auto* a = doc->find("a");
    check(a && a->array.size() == 5 && a->array[0].number == 1 && a->array[1].number == -25 && a->array[2].boolean &&
          a->array[3].kind == JsonValue::Kind::boolean && !a->array[3].boolean && a->array[4].kind == JsonValue::Kind::null, "array members");
    check(doc->find("s")->string == "\xc3\xa9\xf0\x9f\x98\x80\n\"", "escapes and surrogate pairs become UTF-8");
    check(doc->find("o")->kind == JsonValue::Kind::object && !doc->find("missing") && !a->find("a"), "lookups");
    for (const char* bad : {"", "{", "[1,]", "{\"a\":1,}", "{\"a\":1 \"b\":2}", "{\"a\":1,\"a\":2}", "01", "1.", "-", "tru",
                            "\"\\x\"", "\"\\ud800\"", "\"a\nb\"", "[1] 2", "// c\n1", "1e999"}) {
        why.clear();
        check(!parse_json(bad, why) && !why.empty(), "malformed JSON is rejected with a reason");
    }
    check(!parse_json(std::string(40, '[') + std::string(40, ']'), why), "deep nesting is rejected");
}

void verified_lists() {
    using atlan::shell::parse_verified_list;
    const std::string a(64, 'a'), b(64, 'b');
    std::string why;
    const auto list = parse_verified_list(R"({"format": 1, "updated": "2026-11-26", "builds": [
        {"name": "AppA", "version": "1.2.3", "sha256": ")" + a + R"("},
        {"name": "AppB", "version": "4.5", "sha256": ")" + b + R"("}]})", why);
    check(list && list->updated == "2026-11-26" && list->builds.size() == 2, "a list");
    check(list->find(b) && list->find(b)->name == "AppB" && list->find(b)->version == "4.5" && !list->find(std::string(64, 'c')),
          "builds are found by digest");
    check(parse_verified_list(R"({"format": 1, "updated": "2026-01-01", "builds": []})", why).has_value(), "an empty list is a list");
    const auto rejects = [&](const std::string& doc, const char* message) {
        why.clear();
        check(!parse_verified_list(doc, why) && !why.empty(), message);
    };
    const auto entry = [](const std::string& fields) { return R"({"format": 1, "updated": "2026-11-26", "builds": [{)" + fields + "}]}"; };
    rejects(R"({"format": 2, "updated": "2026-11-26", "builds": []})", "a newer format");
    rejects(R"({"format": 1, "updated": "26.11.26", "builds": []})", "a malformed date");
    rejects(R"({"format": 1, "updated": "2026-11-26", "builds": {}})", "builds that are no array");
    rejects(R"({"format": 1, "updated": "2026-11-26", "builds": [], "url": "x"})", "an unknown top-level key");
    rejects(entry(R"("name": "A", "version": "1", "sha256": ")" + a + R"(", "url": "https://example.invalid/a.apk")"), "a download address");
    rejects(entry(R"("name": "A", "version": "1", "sha256": ")" + std::string(64, 'A') + R"(")"), "an upper-case digest");
    rejects(entry(R"("name": "A", "version": "1", "sha256": "abc")"), "a short digest");
    rejects(entry(R"("version": "1", "sha256": ")" + a + R"(")"), "a build without a name");
    rejects(R"({"format": 1, "updated": "2026-11-26", "builds": [{"name": "A", "version": "1", "sha256": ")" + a +
                R"("}, {"name": "B", "version": "2", "sha256": ")" + a + R"("}]})", "a repeated digest");
    // The file the launcher fetches from this repository must stay valid (and so free of any other key).
    std::ifstream file(ATLAN_VERIFIED_LIST);
    const std::string published((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());
    why.clear();
    const auto repository = parse_verified_list(published, why);
    if (!repository) throw std::runtime_error("adapters/atlan/verified_versions.json: " + why);
    check(!repository->builds.empty(), "the published list names at least one build");
}
}

int main() {
    const fs::path scratch = fs::temp_directory_path() / ("atlan-package-tests-" + std::to_string(::getpid()));
    try {
        fs::remove_all(scratch);
        inflating();
        packages();
        discovery(scratch);
        data_status(scratch);
        digests();
        json();
        verified_lists();
        fs::remove_all(scratch);
        std::cout << "package tests passed: " << checks << " checks\n";
    } catch (const std::exception& error) {
        std::cerr << "package test failed: " << error.what() << '\n';
        fs::remove_all(scratch);
        return 1;
    }
}
