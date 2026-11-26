// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include <cctype>
#include <cmath>
#include <cstdlib>
#include <optional>
#include <string>
#include <string_view>
#include <utility>

namespace atlan::shell {

struct GeoPoint {
    double latitude{};   // degrees, north positive
    double longitude{};  // degrees, east positive
};

// A position typed or pasted by a person: two decimal numbers in degrees, latitude first, separated by anything
// that is not part of a number ("37.5665, 126.978", "37.5665 126.978"). A hemisphere letter after a number
// (N S E W, as map apps copy them: "37.5665° N, 126.978° E") gives its sign, and E/W before N/S swaps the pair.
// nullopt when there are not exactly two numbers or a value is out of range.
[[nodiscard]] inline std::optional<GeoPoint> parse_coordinate(std::string_view text) {
    double values[2]{};
    char hemispheres[2]{};
    int count = 0;
    std::size_t i = 0;
    while (i < text.size()) {
        const char c = text[i];
        const bool sign = (c == '-' || c == '+') && i + 1 < text.size() &&
                          (std::isdigit(static_cast<unsigned char>(text[i + 1])) || text[i + 1] == '.');
        if (!sign && !std::isdigit(static_cast<unsigned char>(c)) && !(c == '.' && i + 1 < text.size() &&
                                                                      std::isdigit(static_cast<unsigned char>(text[i + 1])))) {
            const char up = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
            if (up == 'N' || up == 'S' || up == 'E' || up == 'W') {
                if (count == 0 || hemispheres[count - 1] != 0) return std::nullopt;  // a letter without its number
                hemispheres[count - 1] = up;
            } else if (std::isalpha(static_cast<unsigned char>(c))) {
                return std::nullopt;
            }
            ++i;
            continue;
        }
        if (count == 2) return std::nullopt;
        std::size_t end = i + 1;
        bool dot = c == '.';
        while (end < text.size() && (std::isdigit(static_cast<unsigned char>(text[end])) || (text[end] == '.' && !dot))) {
            dot = dot || text[end] == '.';
            ++end;
        }
        const std::string number(text.substr(i, end - i));
        values[count++] = std::strtod(number.c_str(), nullptr);
        i = end;
    }
    if (count != 2) return std::nullopt;
    const auto east_west = [](char h) { return h == 'E' || h == 'W'; };
    const auto north_south = [](char h) { return h == 'N' || h == 'S'; };
    if (east_west(hemispheres[0]) || north_south(hemispheres[1])) {
        if (east_west(hemispheres[1]) || north_south(hemispheres[0])) return std::nullopt;  // both on one axis
        std::swap(values[0], values[1]);
        std::swap(hemispheres[0], hemispheres[1]);
    }
    for (int k = 0; k < 2; ++k) {
        if ((hemispheres[k] == 'S' || hemispheres[k] == 'W')) {
            if (values[k] < 0) return std::nullopt;  // "-37 S" says the sign twice
            values[k] = -values[k];
        }
    }
    if (!std::isfinite(values[0]) || !std::isfinite(values[1]) || std::fabs(values[0]) > 90 || std::fabs(values[1]) > 180)
        return std::nullopt;
    return GeoPoint{values[0], values[1]};
}

}  // namespace atlan::shell
