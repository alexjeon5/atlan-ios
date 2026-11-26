// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include "atlan/native_functions.hpp"
#include <cstdint>
#include <optional>
#include <span>
#include <vector>

// Host versions of the integer geometry the app's map code uses to clip a rectangle against a quadrilateral (an
// orientation test, a segment relation, an even-odd point-in-polygon test and the bounding box of the overlap).
// They reproduce the app's results exactly, including its 32-bit wrapping arithmetic and the cases where it
// compares x only; they were written from a description of the behaviour, not from its code.
//
// Located through the embedder's signature file ("NAME symbol EXPORTED" lines) under the member names below, and
// verified against the guest's own functions before they are replaced.
namespace atlan::shell::geometry {
struct Point {
    std::int32_t x{}, y{};
    friend bool operator==(const Point&, const Point&) = default;
};
// Sign of the cross product (p2 - p1) x (p3 - p1), computed modulo 2^32: 1, 0 or -1.
[[nodiscard]] int orientation(Point p1, Point p2, Point p3) noexcept;
// 1: segments p1p2 and p3p4 cross properly; 0: they touch or overlap; -1: neither (by the app's own rules).
[[nodiscard]] int segment_relation(Point p1, Point p2, Point p3, Point p4) noexcept;
// Even-odd test of `p` against the polygon `points` (at least one point): 1 inside, 0 outside.
[[nodiscard]] int inside_polygon(std::span<const Point> points, Point p) noexcept;
// The overlap of the axis-aligned rectangle with corners a and b and the quadrilateral `quad`.
struct Overlap {
    bool any{};
    bool corners{};    // three or more rectangle corners lie in the quad: the outputs are a and b
    Point low, high;   // what the outputs hold when `any` (outputs that alias the inputs aside)
    std::vector<Point> points;  // otherwise folded into the outputs in this order, after seeding them with b and a
    unsigned crossings{};       // edge intersections among `points` (the app has room for 8)
};
[[nodiscard]] Overlap rectangle_overlap(std::span<const Point, 4> quad, Point a, Point b) noexcept;

// Member names: "orientation", "segment_relation", "inside_polygon", "rectangle_overlap".
[[nodiscard]] NativeSet quad_geometry_set();
}
