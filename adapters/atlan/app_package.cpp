// SPDX-License-Identifier: GPL-2.0-or-later
#include "adapters/atlan/app_package.hpp"
#include <algorithm>
#include <array>
#include <cctype>
#include <filesystem>
#include <fstream>
#include <optional>
#include <stdexcept>
#include <string_view>

namespace atlan::shell {
namespace {
namespace fs = std::filesystem;
[[noreturn]] void fail(const std::string& why) { throw std::runtime_error(why); }

std::uint32_t le16(std::span<const std::byte> data, std::size_t at) {
    if (at + 2 > data.size()) fail("package is truncated");
    return std::to_integer<std::uint32_t>(data[at]) | std::to_integer<std::uint32_t>(data[at + 1]) << 8;
}
std::uint32_t le32(std::span<const std::byte> data, std::size_t at) {
    if (at + 4 > data.size()) fail("package is truncated");
    return le16(data, at) | le16(data, at + 2) << 16;
}

constexpr std::uint32_t local_magic = 0x04034b50, central_magic = 0x02014b50, end_magic = 0x06054b50;
constexpr std::size_t dat_header = 12;  // XOR key, version bytes, payload size (little endian)

bool starts_zip(std::span<const std::byte> data) { return data.size() >= 4 && le32(data, 0) == local_magic; }

// The .dat payload size when `head` starts one that fits in `file_size`.
std::optional<std::uint32_t> dat_payload(std::span<const std::byte> head, std::uint64_t file_size) {
    if (head.size() < dat_header + 4) return std::nullopt;
    const std::uint32_t size = le32(head, 8);
    if (size < 4 || dat_header + std::uint64_t{size} > file_size) return std::nullopt;
    std::array<std::byte, 4> first{};
    for (unsigned i = 0; i < 4; ++i) first[i] = head[dat_header + i] ^ head[i];
    if (!starts_zip(first)) return std::nullopt;
    return size;
}

// ---- DEFLATE ----
struct Huffman {
    std::array<std::uint16_t, 16> count{};    // codes of each length
    std::array<std::uint16_t, 320> symbol{};  // symbols ordered by code
};

void build(Huffman& table, std::span<const std::uint8_t> lengths) {
    table.count.fill(0);
    for (auto length : lengths) ++table.count[length];
    int left = 1;
    for (unsigned length = 1; length < 16; ++length) {
        left = left * 2 - table.count[length];
        if (left < 0) fail("package has a bad compressed stream (over-subscribed code)");
    }
    std::array<std::uint16_t, 16> offset{};
    for (unsigned length = 1; length < 15; ++length) offset[length + 1] = offset[length] + table.count[length];
    for (std::size_t symbol = 0; symbol < lengths.size(); ++symbol)
        if (lengths[symbol] != 0) table.symbol[offset[lengths[symbol]]++] = static_cast<std::uint16_t>(symbol);
}

class Inflater {
public:
    Inflater(std::span<const std::byte> input, std::size_t size) : in_(input), size_(size) { out_.reserve(size); }

    std::vector<std::byte> run() {
        bool last = false;
        while (!last) {
            last = bits(1) != 0;
            switch (bits(2)) {
            case 0: stored(); break;
            case 1: fixed(); break;
            case 2: dynamic(); break;
            default: fail("package has a bad compressed stream (block type 3)");
            }
        }
        if (out_.size() != size_) fail("package entry is shorter than its recorded size");
        return std::move(out_);
    }

private:
    std::uint32_t bits(unsigned need) {
        std::uint32_t value = buffer_;
        while (count_ < need) {
            if (pos_ >= in_.size()) fail("package has a truncated compressed stream");
            value |= std::to_integer<std::uint32_t>(in_[pos_++]) << count_;
            count_ += 8;
        }
        buffer_ = value >> need;
        count_ -= need;
        return value & ((1u << need) - 1);
    }

    void put(std::byte value) {
        if (out_.size() >= size_) fail("package entry is longer than its recorded size");
        out_.push_back(value);
    }

    void stored() {
        buffer_ = 0;
        count_ = 0;
        if (pos_ + 4 > in_.size()) fail("package has a truncated compressed stream");
        const std::uint32_t length = le16(in_, pos_), check = le16(in_, pos_ + 2);
        pos_ += 4;
        if ((length ^ 0xffff) != check) fail("package has a bad compressed stream (stored length)");
        if (pos_ + length > in_.size()) fail("package has a truncated compressed stream");
        if (out_.size() + length > size_) fail("package entry is longer than its recorded size");
        out_.insert(out_.end(), in_.begin() + static_cast<std::ptrdiff_t>(pos_), in_.begin() + static_cast<std::ptrdiff_t>(pos_ + length));
        pos_ += length;
    }

    int decode(const Huffman& table) {
        int code = 0, first = 0, index = 0;
        for (unsigned length = 1; length < 16; ++length) {
            code |= static_cast<int>(bits(1));
            const int count = table.count[length];
            if (code - count < first) return table.symbol[static_cast<std::size_t>(index + (code - first))];
            index += count;
            first = (first + count) << 1;
            code <<= 1;
        }
        fail("package has a bad compressed stream (unused code)");
    }

    void codes(const Huffman& lengths, const Huffman& distances) {
        static constexpr std::array<std::uint16_t, 29> length_base{3, 4, 5, 6, 7, 8, 9, 10, 11, 13, 15, 17, 19, 23, 27, 31,
                                                                   35, 43, 51, 59, 67, 83, 99, 115, 131, 163, 195, 227, 258};
        static constexpr std::array<std::uint8_t, 29> length_extra{0, 0, 0, 0, 0, 0, 0, 0, 1, 1, 1, 1, 2, 2, 2,
                                                                   2, 3, 3, 3, 3, 4, 4, 4, 4, 5, 5, 5, 5, 0};
        static constexpr std::array<std::uint16_t, 30> distance_base{1, 2, 3, 4, 5, 7, 9, 13, 17, 25, 33, 49, 65, 97, 129,
                                                                     193, 257, 385, 513, 769, 1025, 1537, 2049, 3073,
                                                                     4097, 6145, 8193, 12289, 16385, 24577};
        static constexpr std::array<std::uint8_t, 30> distance_extra{0, 0, 0, 0, 1, 1, 2, 2, 3, 3, 4, 4, 5, 5, 6,
                                                                     6, 7, 7, 8, 8, 9, 9, 10, 10, 11, 11, 12, 12, 13, 13};
        for (;;) {
            int symbol = decode(lengths);
            if (symbol < 256) { put(static_cast<std::byte>(symbol)); continue; }
            if (symbol == 256) return;
            symbol -= 257;
            if (symbol >= 29) fail("package has a bad compressed stream (length symbol)");
            const std::size_t length = length_base[static_cast<std::size_t>(symbol)] + bits(length_extra[static_cast<std::size_t>(symbol)]);
            const int which = decode(distances);
            if (which >= 30) fail("package has a bad compressed stream (distance symbol)");
            const std::size_t distance = distance_base[static_cast<std::size_t>(which)] + bits(distance_extra[static_cast<std::size_t>(which)]);
            if (distance > out_.size()) fail("package has a bad compressed stream (distance too far)");
            if (out_.size() + length > size_) fail("package entry is longer than its recorded size");
            for (std::size_t i = 0; i < length; ++i) {
                const std::byte value = out_[out_.size() - distance];
                out_.push_back(value);
            }
        }
    }

    void fixed() {
        static const auto tables = [] {
            std::pair<Huffman, Huffman> result;
            std::array<std::uint8_t, 288> lengths{};
            for (std::size_t i = 0; i < 288; ++i) lengths[i] = i < 144 ? 8 : i < 256 ? 9 : i < 280 ? 7 : 8;
            build(result.first, lengths);
            std::array<std::uint8_t, 30> distances{};
            distances.fill(5);
            build(result.second, distances);
            return result;
        }();
        codes(tables.first, tables.second);
    }

    void dynamic() {
        static constexpr std::array<std::uint8_t, 19> order{16, 17, 18, 0, 8, 7, 9, 6, 10, 5, 11, 4, 12, 3, 13, 2, 14, 1, 15};
        const unsigned literal_count = bits(5) + 257, distance_count = bits(5) + 1, code_count = bits(4) + 4;
        if (literal_count > 286 || distance_count > 30) fail("package has a bad compressed stream (code counts)");
        std::array<std::uint8_t, 19> code_lengths{};
        for (unsigned i = 0; i < code_count; ++i) code_lengths[order[i]] = static_cast<std::uint8_t>(bits(3));
        Huffman code_table;
        build(code_table, code_lengths);
        std::array<std::uint8_t, 316> lengths{};
        unsigned index = 0;
        while (index < literal_count + distance_count) {
            const int symbol = decode(code_table);
            if (symbol < 16) { lengths[index++] = static_cast<std::uint8_t>(symbol); continue; }
            std::uint8_t value = 0;
            unsigned repeat = 0;
            if (symbol == 16) {
                if (index == 0) fail("package has a bad compressed stream (repeat without a length)");
                value = lengths[index - 1];
                repeat = 3 + bits(2);
            } else if (symbol == 17) {
                repeat = 3 + bits(3);
            } else {
                repeat = 11 + bits(7);
            }
            if (index + repeat > literal_count + distance_count) fail("package has a bad compressed stream (too many lengths)");
            while (repeat-- != 0) lengths[index++] = value;
        }
        if (lengths[256] == 0) fail("package has a bad compressed stream (no end code)");
        Huffman literal_table, distance_table;
        build(literal_table, std::span<const std::uint8_t>(lengths.data(), literal_count));
        build(distance_table, std::span<const std::uint8_t>(lengths.data() + literal_count, distance_count));
        codes(literal_table, distance_table);
    }

    std::span<const std::byte> in_;
    std::size_t size_;
    std::size_t pos_{};
    std::uint32_t buffer_{};
    unsigned count_{};
    std::vector<std::byte> out_;
};

// ---- ZIP ----
struct Entry {
    std::string name;
    std::uint32_t method{}, flags{}, crc{}, compressed{}, size{}, local{};
};

std::vector<Entry> central_directory(std::span<const std::byte> zip) {
    if (zip.size() < 22) fail("package is not a ZIP archive");
    std::size_t end = zip.size() - 22;
    const std::size_t lowest = zip.size() > 22 + 0xffff ? zip.size() - 22 - 0xffff : 0;
    while (le32(zip, end) != end_magic) {
        if (end == lowest) fail("package has no ZIP directory");
        --end;
    }
    const std::uint32_t count = le16(zip, end + 10), offset = le32(zip, end + 16);
    std::vector<Entry> entries;
    std::size_t at = offset;
    for (std::uint32_t i = 0; i < count; ++i) {
        if (le32(zip, at) != central_magic) fail("package has a damaged ZIP directory");
        Entry entry;
        entry.flags = le16(zip, at + 8);
        entry.method = le16(zip, at + 10);
        entry.crc = le32(zip, at + 16);
        entry.compressed = le32(zip, at + 20);
        entry.size = le32(zip, at + 24);
        const std::uint32_t name = le16(zip, at + 28), extra = le16(zip, at + 30), comment = le16(zip, at + 32);
        entry.local = le32(zip, at + 42);
        if (at + 46 + name > zip.size()) fail("package has a damaged ZIP directory");
        entry.name.assign(reinterpret_cast<const char*>(zip.data() + at + 46), name);
        entries.push_back(std::move(entry));
        at += 46 + name + extra + comment;
    }
    return entries;
}

std::vector<std::byte> extract(std::span<const std::byte> zip, const Entry& entry) {
    if (entry.flags & 1) fail(entry.name + " is encrypted");
    if (le32(zip, entry.local) != local_magic) fail(entry.name + " has a damaged ZIP header");
    const std::size_t data = entry.local + 30 + le16(zip, entry.local + 26) + le16(zip, entry.local + 28);
    if (data + entry.compressed > zip.size()) fail(entry.name + " is truncated");
    const auto input = zip.subspan(data, entry.compressed);
    std::vector<std::byte> bytes;
    if (entry.method == 0) {
        if (entry.compressed != entry.size) fail(entry.name + " has inconsistent sizes");
        bytes.assign(input.begin(), input.end());
    } else if (entry.method == 8) {
        bytes = inflate_raw(input, entry.size);
    } else {
        fail(entry.name + " uses unsupported compression method " + std::to_string(entry.method));
    }
    if (crc32(bytes) != entry.crc) fail(entry.name + " fails its CRC check");
    return bytes;
}

std::string lower(std::string_view text) {
    std::string result(text);
    for (auto& c : result) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return result;
}

// Prefers the app's own library (libatlan*) and the newer 32-bit ARM ABI.
const Entry& native_library(const std::vector<Entry>& entries) {
    const Entry* best = nullptr;
    int best_rank = -1;
    std::size_t arm32 = 0;
    std::string others;
    for (const auto& entry : entries) {
        const std::string_view name = entry.name;
        if (!name.starts_with("lib/") || !name.ends_with(".so")) continue;
        const auto slash = name.find('/', 4);
        if (slash == std::string_view::npos || name.find('/', slash + 1) != std::string_view::npos) continue;
        const auto abi = name.substr(4, slash - 4);
        if (abi != "armeabi-v7a" && abi != "armeabi") {
            if (others.empty()) others = entry.name;
            continue;
        }
        ++arm32;
        const int rank = (lower(name.substr(slash + 1)).starts_with("libatlan") ? 2 : 0) + (abi == "armeabi-v7a" ? 1 : 0);
        if (rank > best_rank) { best = &entry; best_rank = rank; }
    }
    if (!best) fail(others.empty() ? "package has no native library" : "package has no 32-bit ARM library (found " + others + ")");
    if (best_rank < 2 && arm32 > 1) fail("package has several 32-bit ARM libraries and none is libatlan*.so");
    return *best;
}

std::optional<std::vector<std::byte>> read_head(const fs::path& path, std::size_t bytes) {
    std::ifstream input(path, std::ios::binary);
    if (!input) return std::nullopt;
    std::vector<std::byte> head(bytes);
    input.read(reinterpret_cast<char*>(head.data()), static_cast<std::streamsize>(bytes));
    head.resize(static_cast<std::size_t>(input.gcount()));
    return head;
}

std::optional<fs::path> child_named(const fs::path& folder, std::string_view name) {
    std::error_code error;
    for (fs::directory_iterator it(folder, error), end; !error && it != end; it.increment(error))
        if (it->is_directory(error) && lower(it->path().filename().string()) == name) return it->path();
    return std::nullopt;
}
}

std::uint32_t crc32(std::span<const std::byte> data) {
    static const auto table = [] {
        std::array<std::uint32_t, 256> result{};
        for (std::uint32_t i = 0; i < 256; ++i) {
            std::uint32_t value = i;
            for (int bit = 0; bit < 8; ++bit) value = value & 1 ? 0xedb88320u ^ (value >> 1) : value >> 1;
            result[i] = value;
        }
        return result;
    }();
    std::uint32_t crc = 0xffffffffu;
    for (auto byte : data) crc = table[(crc ^ std::to_integer<std::uint32_t>(byte)) & 255] ^ (crc >> 8);
    return crc ^ 0xffffffffu;
}

std::vector<std::byte> inflate_raw(std::span<const std::byte> input, std::size_t size) { return Inflater(input, size).run(); }

bool is_package(std::span<const std::byte> head, std::uint64_t file_size) {
    return starts_zip(head) || dat_payload(head, file_size).has_value();
}

GuestPackage open_package(std::span<const std::byte> file) {
    GuestPackage package;
    std::vector<std::byte> decoded;
    std::span<const std::byte> zip = file;
    if (!starts_zip(file)) {
        const auto size = dat_payload(file, file.size());
        if (!size) fail("not an APK or an encoded .dat package");
        decoded.assign(file.begin() + dat_header, file.begin() + static_cast<std::ptrdiff_t>(dat_header + *size));
        // The app's DatToApk: every byte of whole 4-byte groups is XORed with the key; a tail of 1-3 bytes is plain.
        for (std::size_t i = 0; i < decoded.size() / 4 * 4; ++i) decoded[i] ^= file[i % 4];
        for (unsigned i = 4; i < 8; ++i)
            package.version += (i > 4 ? "." : "") + std::to_string(std::to_integer<unsigned>(file[i]));
        zip = decoded;
    }
    const auto entries = central_directory(zip);
    const auto version = std::find_if(entries.begin(), entries.end(), [](const Entry& e) { return e.name == "assets/version.txt"; });
    if (version != entries.end() && version->size <= 64) {
        const auto bytes = extract(zip, *version);
        std::string text(reinterpret_cast<const char*>(bytes.data()), bytes.size());
        while (!text.empty() && std::isspace(static_cast<unsigned char>(text.back()))) text.pop_back();
        if (!text.empty()) package.version = text;
    }
    const Entry& library = native_library(entries);
    package.entry = library.name;
    package.library = extract(zip, library);
    // The Java side's method table, so native methods bound by name can be checked against their declared signature.
    // A damaged dex only loses that check, not the run.
    for (const auto& entry : entries) {
        const std::string_view name = entry.name;
        if (!name.starts_with("classes") || !name.ends_with(".dex") || name.find('/') != std::string_view::npos) continue;
        try {
            auto methods = dex_methods(extract(zip, entry));
            package.java_methods.insert(package.java_methods.end(), std::make_move_iterator(methods.begin()), std::make_move_iterator(methods.end()));
        } catch (const std::runtime_error&) {
        }
    }
    return package;
}

std::string find_package(const std::string& data_root) {
    if (data_root.empty()) return {};
    const auto atlan = child_named(data_root, "atlan");
    return atlan ? find_package_in(atlan->string()) : std::string{};
}

std::string find_package_in(const std::string& folder) {
    if (folder.empty()) return {};
    const auto atlan = std::optional<fs::path>(folder);
    std::vector<fs::path> candidates;
    std::error_code error;
    const auto collect = [&](const fs::path& folder, bool dat) {
        for (fs::directory_iterator it(folder, error), end; !error && it != end; it.increment(error)) {
            const std::string name = lower(it->path().filename().string());
            if (!it->is_regular_file(error)) continue;
            if (name.ends_with(".apk") || (dat && name == "atlan.dat")) candidates.push_back(it->path());
        }
        error.clear();
    };
    if (const auto app = child_named(*atlan, "app")) collect(*app, true);
    collect(*atlan, false);
    std::string best;
    fs::file_time_type newest{};
    for (const auto& path : candidates) {
        const auto size = fs::file_size(path, error);
        if (error) { error.clear(); continue; }
        const auto head = read_head(path, dat_header + 4);
        if (!head || !is_package(*head, size)) continue;
        const auto time = fs::last_write_time(path, error);
        if (error) { error.clear(); continue; }
        if (best.empty() || time > newest) { best = path.string(); newest = time; }
    }
    return best;
}
}
