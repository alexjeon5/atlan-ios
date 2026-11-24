// SPDX-License-Identifier: GPL-2.0-or-later
#include "atlan/libc_extra.hpp"
#include "atlan/abi.hpp"
#include <array>
#include <bit>
#include <cerrno>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace atlan {
namespace {
constexpr std::size_t max_string = 1U << 20;
bool read_text(ImportCall& call, std::uint32_t address, std::string& out) {
    return read_guest_string(call.runtime().memory, address, max_string, out);
}
bool put_word(ImportCall& call, std::uint32_t address, std::uint32_t value) {
    std::array<std::byte, 4> bytes{};
    for (unsigned i = 0; i < 4; ++i) bytes[i] = std::byte((value >> (8 * i)) & 255);
    return call.runtime().memory.write(address, bytes);
}
double double_arg(ImportCall& call, unsigned index) {  // soft-float: low word in the first register
    return std::bit_cast<double>(std::uint64_t{call.arg(index)} | std::uint64_t{call.arg(index + 1)} << 32);
}
float float_arg(ImportCall& call, unsigned index) { return std::bit_cast<float>(call.arg(index)); }
void double_result(ImportCall& call, double value) {
    const auto bits = std::bit_cast<std::uint64_t>(value);
    call.runtime().cpu.registers[0] = static_cast<std::uint32_t>(bits);
    call.runtime().cpu.registers[1] = static_cast<std::uint32_t>(bits >> 32);
}
void float_result(ImportCall& call, float value) { call.set_result(std::bit_cast<std::uint32_t>(value)); }
}
void install_libc_extra(ImportDispatcher& dispatcher) {
    // void* memchr(const void* s, int c, size_t n)
    dispatcher.bind("memchr", [](ImportCall& call) -> std::optional<std::string> {
        const auto s = call.arg(0), n = call.arg(2);
        const auto wanted = static_cast<std::uint8_t>(call.arg(1));
        if (n > (1U << 28)) return "memchr size exceeds service bound";
        std::vector<std::byte> bytes(n);
        if (n != 0 && !call.runtime().memory.read(s, bytes)) return "memchr read left mapped guest memory";
        for (std::uint32_t i = 0; i < n; ++i)
            if (std::to_integer<std::uint8_t>(bytes[i]) == wanted) { call.set_result(s + i); return std::nullopt; }
        call.set_result(0);
        return std::nullopt;
    });
    // char* strstr(const char* haystack, const char* needle)
    dispatcher.bind("strstr", [](ImportCall& call) -> std::optional<std::string> {
        std::string hay, needle;
        if (!read_text(call, call.arg(0), hay) || !read_text(call, call.arg(1), needle)) return "strstr string is unreadable or too long";
        const auto at = hay.find(needle);
        call.set_result(at == std::string::npos ? 0 : call.arg(0) + static_cast<std::uint32_t>(at));
        return std::nullopt;
    });
    // size_t strcspn(s, reject) / char* strpbrk(s, accept)
    dispatcher.bind("strcspn", [](ImportCall& call) -> std::optional<std::string> {
        std::string s, set;
        if (!read_text(call, call.arg(0), s) || !read_text(call, call.arg(1), set)) return "strcspn string is unreadable or too long";
        const auto at = s.find_first_of(set);
        call.set_result(static_cast<std::uint32_t>(at == std::string::npos ? s.size() : at));
        return std::nullopt;
    });
    dispatcher.bind("strpbrk", [](ImportCall& call) -> std::optional<std::string> {
        std::string s, set;
        if (!read_text(call, call.arg(0), s) || !read_text(call, call.arg(1), set)) return "strpbrk string is unreadable or too long";
        const auto at = s.find_first_of(set);
        call.set_result(at == std::string::npos ? 0 : call.arg(0) + static_cast<std::uint32_t>(at));
        return std::nullopt;
    });
    // char* strncat(dst, src, n): appends at most n bytes of src plus a terminator.
    dispatcher.bind("strncat", [](ImportCall& call) -> std::optional<std::string> {
        std::string dst, src;
        if (!read_text(call, call.arg(0), dst) || !read_text(call, call.arg(1), src)) return "strncat string is unreadable or too long";
        src.resize(std::min<std::size_t>(src.size(), call.arg(2)));
        std::vector<std::byte> bytes(src.size() + 1, std::byte{0});
        std::memcpy(bytes.data(), src.data(), src.size());
        if (!call.runtime().memory.write(call.arg(0) + static_cast<std::uint32_t>(dst.size()), bytes)) return "strncat destination is not writable";
        call.set_result(call.arg(0));
        return std::nullopt;
    });
    // char* strtok(char* s, const char* delim): the position lives in host state (one per guest, like libc's static).
    const auto token_position = std::make_shared<std::uint32_t>(0);
    dispatcher.bind("strtok", [token_position](ImportCall& call) -> std::optional<std::string> {
        std::string delimiters;
        if (!read_text(call, call.arg(1), delimiters)) return "strtok delimiters are unreadable";
        std::uint32_t cursor = call.arg(0) != 0 ? call.arg(0) : *token_position;
        if (cursor == 0) { call.set_result(0); return std::nullopt; }
        std::string rest;
        if (!read_text(call, cursor, rest)) return "strtok string is unreadable or too long";
        const auto start = rest.find_first_not_of(delimiters);
        if (start == std::string::npos) { *token_position = 0; call.set_result(0); return std::nullopt; }
        const auto end = rest.find_first_of(delimiters, start);
        if (end == std::string::npos) *token_position = cursor + static_cast<std::uint32_t>(rest.size());
        else {
            const std::array<std::byte, 1> nul{std::byte{0}};
            if (!call.runtime().memory.write(cursor + static_cast<std::uint32_t>(end), nul)) return "strtok string is not writable";
            *token_position = cursor + static_cast<std::uint32_t>(end) + 1;
        }
        call.set_result(cursor + static_cast<std::uint32_t>(start));
        return std::nullopt;
    });
    // Number parsing with the host C library in the C locale (bases 2-36 as libc defines them).
    for (const bool is_unsigned : {false, true}) {
        dispatcher.bind(is_unsigned ? "strtoul" : "strtol", [is_unsigned](ImportCall& call) -> std::optional<std::string> {
            std::string text;
            if (!read_text(call, call.arg(0), text)) return "strtol string is unreadable or too long";
            char* end = nullptr;
            errno = 0;
            const long long value = is_unsigned ? static_cast<long long>(std::strtoull(text.c_str(), &end, static_cast<int>(call.arg(2))))
                                                : std::strtoll(text.c_str(), &end, static_cast<int>(call.arg(2)));
            const bool overflow = errno == ERANGE;
            std::uint32_t result = static_cast<std::uint32_t>(value);
            if (is_unsigned) { if (overflow || static_cast<unsigned long long>(value) > 0xffffffffULL) result = 0xffffffffU; }
            else if (overflow || value > 0x7fffffffLL || value < -0x80000000LL) result = value < 0 ? 0x80000000U : 0x7fffffffU;
            if (call.arg(1) != 0 && !put_word(call, call.arg(1), call.arg(0) + static_cast<std::uint32_t>(end - text.c_str())))
                return "strtol end pointer is not writable";
            call.set_result(result);
            return std::nullopt;
        });
    }
    dispatcher.bind("atol", [](ImportCall& call) -> std::optional<std::string> {
        std::string text;
        if (!read_text(call, call.arg(0), text)) return "atol string is unreadable or too long";
        const long long value = std::strtoll(text.c_str(), nullptr, 10);
        call.set_result(static_cast<std::uint32_t>(value > 0x7fffffffLL ? 0x7fffffffLL : value < -0x80000000LL ? -0x80000000LL : value));
        return std::nullopt;
    });
    dispatcher.bind("strtod", [](ImportCall& call) -> std::optional<std::string> {
        std::string text;
        if (!read_text(call, call.arg(0), text)) return "strtod string is unreadable or too long";
        char* end = nullptr;
        const double value = std::strtod(text.c_str(), &end);
        if (call.arg(1) != 0 && !put_word(call, call.arg(1), call.arg(0) + static_cast<std::uint32_t>(end - text.c_str())))
            return "strtod end pointer is not writable";
        double_result(call, value);
        return std::nullopt;
    });
    // Character classes and case mapping: C locale, so only ASCII letters change; wide classes see ASCII only.
    dispatcher.bind("toupper", [](ImportCall& call) -> std::optional<std::string> {
        const auto c = call.arg(0); call.set_result(c >= 'a' && c <= 'z' ? c - 32 : c); return std::nullopt; });
    dispatcher.bind("tolower", [](ImportCall& call) -> std::optional<std::string> {
        const auto c = call.arg(0); call.set_result(c >= 'A' && c <= 'Z' ? c + 32 : c); return std::nullopt; });
    dispatcher.bind("towupper", [](ImportCall& call) -> std::optional<std::string> {
        const auto c = call.arg(0); call.set_result(c >= 'a' && c <= 'z' ? c - 32 : c); return std::nullopt; });
    dispatcher.bind("towlower", [](ImportCall& call) -> std::optional<std::string> {
        const auto c = call.arg(0); call.set_result(c >= 'A' && c <= 'Z' ? c + 32 : c); return std::nullopt; });
    const std::pair<const char*, bool (*)(std::uint32_t)> wide_classes[] = {
        {"iswalpha", [](std::uint32_t c) { return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z'); }},
        {"iswcntrl", [](std::uint32_t c) { return c < 32 || c == 127; }},
        {"iswdigit", [](std::uint32_t c) { return c >= '0' && c <= '9'; }},
        {"iswlower", [](std::uint32_t c) { return c >= 'a' && c <= 'z'; }},
        {"iswupper", [](std::uint32_t c) { return c >= 'A' && c <= 'Z'; }},
        {"iswprint", [](std::uint32_t c) { return c >= 32 && c < 127; }},
        {"iswspace", [](std::uint32_t c) { return c == ' ' || (c >= 9 && c <= 13); }},
        {"iswxdigit", [](std::uint32_t c) { return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F'); }},
        {"iswpunct", [](std::uint32_t c) { return c > 32 && c < 127 && !((c >= '0' && c <= '9') || (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z')); }},
    };
    for (const auto& [name, test] : wide_classes)
        dispatcher.bind(name, [test = test](ImportCall& call) -> std::optional<std::string> { call.set_result(test(call.arg(0)) ? 1 : 0); return std::nullopt; });
    // libm: one-argument and two-argument forms, double (r0:r1[, r2:r3]) and float (r0[, r1]).
    const std::pair<const char*, double (*)(double)> unary_d[] = {
        {"sin", std::sin}, {"cos", std::cos}, {"tan", std::tan}, {"acos", std::acos}, {"atan", std::atan},
        {"sqrt", std::sqrt}, {"floor", std::floor}};
    for (const auto& [name, fn] : unary_d)
        dispatcher.bind(name, [fn = fn](ImportCall& call) -> std::optional<std::string> { double_result(call, fn(double_arg(call, 0))); return std::nullopt; });
    const std::pair<const char*, float (*)(float)> unary_f[] = {
        {"sinf", std::sin}, {"cosf", std::cos}, {"tanf", std::tan}, {"acosf", std::acos}, {"asinf", std::asin},
        {"atanf", std::atan}, {"sqrtf", std::sqrt}, {"floorf", std::floor}, {"ceilf", std::ceil}, {"log10f", std::log10}};
    for (const auto& [name, fn] : unary_f)
        dispatcher.bind(name, [fn = fn](ImportCall& call) -> std::optional<std::string> { float_result(call, fn(float_arg(call, 0))); return std::nullopt; });
    dispatcher.bind("atan2", [](ImportCall& call) -> std::optional<std::string> { double_result(call, std::atan2(double_arg(call, 0), double_arg(call, 2))); return std::nullopt; });
    dispatcher.bind("pow", [](ImportCall& call) -> std::optional<std::string> { double_result(call, std::pow(double_arg(call, 0), double_arg(call, 2))); return std::nullopt; });
    dispatcher.bind("atan2f", [](ImportCall& call) -> std::optional<std::string> { float_result(call, std::atan2(float_arg(call, 0), float_arg(call, 1))); return std::nullopt; });
    dispatcher.bind("powf", [](ImportCall& call) -> std::optional<std::string> { float_result(call, std::pow(float_arg(call, 0), float_arg(call, 1))); return std::nullopt; });
    dispatcher.bind("lroundf", [](ImportCall& call) -> std::optional<std::string> {
        call.set_result(static_cast<std::uint32_t>(static_cast<std::int32_t>(std::lround(float_arg(call, 0))))); return std::nullopt; });
    // void qsort(void* base, size_t n, size_t size, int (*cmp)(const void*, const void*)): a merge sort over element
    // addresses that never moves guest data during comparisons, so the guest comparator always sees stable,
    // valid pointers; the permutation is applied once at the end. Safe for inconsistent comparators.
    dispatcher.bind("qsort", [](ImportCall& call) -> std::optional<std::string> {
        const auto base = call.arg(0), count = call.arg(1), size = call.arg(2), compare = call.arg(3);
        if (count < 2) return std::nullopt;
        if (size == 0 || std::uint64_t{count} * size > (1ULL << 28) || std::uint64_t{base} + std::uint64_t{count} * size > 0x100000000ULL)
            return "qsort array size is out of range";
        std::vector<std::byte> data(std::size_t{count} * size);
        if (!call.runtime().memory.read(base, data)) return "qsort array is unreadable";
        std::optional<std::string> failure;
        const auto less_or_equal = [&](std::uint32_t a, std::uint32_t b) {  // true when element a sorts before or equal to b
            if (failure) return true;
            const std::array<std::uint32_t, 2> args{base + a * size, base + b * size};
            const auto outcome = call.dispatcher().call_back(call.runtime(), compare, args);
            if (outcome.stop != Stop::returned) {
                failure = "qsort comparator did not return normally" + (outcome.message.empty() ? std::string() : ": " + outcome.message) +
                          (outcome.import.empty() ? std::string() : " (" + outcome.import + ")");
                return true;
            }
            return static_cast<std::int32_t>(call.runtime().cpu.registers[0]) <= 0;
        };
        std::vector<std::uint32_t> order(count), scratch(count);
        for (std::uint32_t i = 0; i < count; ++i) order[i] = i;
        const std::function<void(std::uint32_t, std::uint32_t)> sort = [&](std::uint32_t lo, std::uint32_t hi) {
            if (hi - lo < 2 || failure) return;
            const auto mid = lo + (hi - lo) / 2;
            sort(lo, mid); sort(mid, hi);
            std::uint32_t i = lo, j = mid, k = lo;
            while (i < mid && j < hi) scratch[k++] = less_or_equal(order[i], order[j]) ? order[i++] : order[j++];
            while (i < mid) scratch[k++] = order[i++];
            while (j < hi) scratch[k++] = order[j++];
            for (k = lo; k < hi; ++k) order[k] = scratch[k];
        };
        sort(0, count);
        if (failure) return failure;
        std::vector<std::byte> sorted(data.size());
        for (std::uint32_t i = 0; i < count; ++i) std::memcpy(sorted.data() + std::size_t{i} * size, data.data() + std::size_t{order[i]} * size, size);
        if (!call.runtime().memory.write(base, sorted)) return "qsort array is not writable";
        return std::nullopt;
    });
    // Process identity and termination. The guest has one process; ending it is reported, never swallowed.
    dispatcher.bind("getpid", [](ImportCall& call) -> std::optional<std::string> { call.set_result(1000); return std::nullopt; });
    dispatcher.bind("abort", [](ImportCall&) -> std::optional<std::string> { return "guest called abort()"; });
    dispatcher.bind("exit", [](ImportCall& call) -> std::optional<std::string> {
        return "guest called exit(" + std::to_string(static_cast<std::int32_t>(call.arg(0))) + ")"; });
    dispatcher.bind("__assert2", [](ImportCall& call) -> std::optional<std::string> {
        std::string file, func, expr;
        read_text(call, call.arg(0), file); read_text(call, call.arg(3), expr); read_text(call, call.arg(2), func);
        return "guest assertion failed: " + expr + " in " + func + " (" + file + ":" + std::to_string(call.arg(1)) + ")";
    });
}
}
