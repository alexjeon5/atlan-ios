// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <vector>

// The guest app as the user installs it: an APK, or the copy the app keeps in its data folder (ATLAN/APP/atlan.dat,
// the APK behind a 12-byte header and a 4-byte XOR key). The ARM32 native library is taken out in memory on every
// run, so a newer package placed in the folder is used as is.
namespace atlan::shell {
struct GuestPackage {
    std::vector<std::byte> library;  // the ARM32 ELF shared object
    std::string entry;               // its path inside the APK, e.g. lib/armeabi/libatlan3dv5.so
    std::string version;             // assets/version.txt, else the .dat header; empty if neither
};

// True when the bytes start an APK (a ZIP local header) or an encoded .dat whose payload starts one. `file_size` is the
// whole file's size when only its head is given.
bool is_package(std::span<const std::byte> head, std::uint64_t file_size);
inline bool is_package(std::span<const std::byte> file) { return is_package(file, file.size()); }

// Takes the ARM32 native library out of a whole package file. Throws std::runtime_error naming what is wrong.
GuestPackage open_package(std::span<const std::byte> file);

// The package in a data root: the newest of ATLAN/APP/atlan.dat, ATLAN/APP/*.apk and ATLAN/*.apk (folder and file
// names in any case) that is a package. Empty when there is none.
std::string find_package(const std::string& data_root);
// The same search inside the ATLAN folder itself, wherever it lies (e.g. a folder the user picked).
std::string find_package_in(const std::string& folder);

// Raw DEFLATE (RFC 1951) decoding into exactly `size` bytes, and the ZIP CRC-32. Exposed for tests.
std::vector<std::byte> inflate_raw(std::span<const std::byte> input, std::size_t size);
std::uint32_t crc32(std::span<const std::byte> data);
}
