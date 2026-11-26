// SPDX-License-Identifier: GPL-2.0-or-later
#include "adapters/atlan/quad_geometry.hpp"
#include <algorithm>
#include <array>
#include <random>
#include <string>
#include <vector>

namespace atlan::shell::geometry {
namespace {
using Words = std::vector<std::uint32_t>;
using Error = std::optional<std::string>;

[[nodiscard]] std::uint32_t u(std::int32_t v) noexcept { return static_cast<std::uint32_t>(v); }
[[nodiscard]] std::int32_t s(std::uint32_t v) noexcept { return static_cast<std::int32_t>(v); }
[[nodiscard]] std::int32_t sub(std::int32_t a, std::int32_t b) noexcept { return s(u(a) - u(b)); }
[[nodiscard]] std::int32_t mul(std::int32_t a, std::int32_t b) noexcept { return s(u(a) * u(b)); }
[[nodiscard]] std::int32_t add(std::int32_t a, std::int32_t b) noexcept { return s(u(a) + u(b)); }
// Truncating division with INT_MIN / -1 wrapping, as the run-time ABI helper does. `d` is never 0 here.
[[nodiscard]] std::int32_t divide(std::int32_t n, std::int32_t d) noexcept {
    return s(static_cast<std::uint32_t>(std::int64_t{n} / std::int64_t{d}));
}

[[nodiscard]] bool load_words(Memory& memory, std::uint32_t at, std::span<std::int32_t> out) {
    std::vector<std::byte> bytes(out.size() * 4);
    if (!memory.read(at, bytes)) return false;
    for (std::size_t i = 0; i < out.size(); ++i) {
        std::uint32_t v = 0;
        for (unsigned k = 0; k < 4; ++k) v |= std::to_integer<std::uint32_t>(bytes[4 * i + k]) << (8 * k);
        out[i] = s(v);
    }
    return true;
}
[[nodiscard]] bool load_point(Memory& memory, std::uint32_t at, Point& p) {
    std::array<std::int32_t, 2> w{};
    if (!load_words(memory, at, w)) return false;
    p = {w[0], w[1]};
    return true;
}
[[nodiscard]] bool store_point(Memory& memory, std::uint32_t at, Point p) {
    std::array<std::byte, 8> bytes{};
    for (unsigned k = 0; k < 4; ++k) {
        bytes[k] = std::byte((u(p.x) >> (8 * k)) & 255U);
        bytes[4 + k] = std::byte((u(p.y) >> (8 * k)) & 255U);
    }
    return memory.write(at, bytes);
}
[[nodiscard]] bool args(ImportCall& call, std::span<std::int32_t> out) {
    for (unsigned i = 0; i < out.size(); ++i) {
        std::uint32_t v = 0;
        if (!call.read_arg(i, v)) return false;
        out[i] = s(v);
    }
    return true;
}
}

int orientation(Point p1, Point p2, Point p3) noexcept {
    const std::int32_t d = sub(mul(sub(p2.x, p1.x), sub(p3.y, p1.y)), mul(sub(p2.y, p1.y), sub(p3.x, p1.x)));
    return d > 0 ? 1 : d < 0 ? -1 : 0;
}

int segment_relation(Point p1, Point p2, Point p3, Point p4) noexcept {
    if (p1.x > p2.x) std::swap(p1, p2);
    if (p3.x > p4.x) std::swap(p3, p4);
    const int a = orientation(p1, p2, p3), b = orientation(p1, p2, p4);
    const int c = orientation(p3, p4, p1), d = orientation(p3, p4, p2);
    if (a * b < 0 && c * d < 0) return 1;
    // Collinear: the app compares only one end against the other segment, and x only.
    if (a == 0 && b == 0) return p1.x <= p4.x ? 0 : -1;
    const auto within = [](std::int32_t lo, std::int32_t v, std::int32_t hi) { return lo <= v && v <= hi ? 0 : -1; };
    if (a == 0) return within(p1.x, p3.x, p2.x);
    if (b == 0) return within(p1.x, p4.x, p2.x);
    if (c == 0) return within(p3.x, p1.x, p4.x);
    if (d == 0) return within(p3.x, p2.x, p4.x);
    return -1;
}

int inside_polygon(std::span<const Point> points, Point p) noexcept {
    std::int32_t left = points[0].x;
    for (const Point& q : points) left = std::min(left, q.x);
    const Point far{sub(left, 10), p.y};
    unsigned crossings = 0;
    const std::size_t n = points.size();
    for (std::size_t i = 0; i < n; ++i) {
        const Point& q0 = points[i];
        const Point& q1 = points[(i + 1) % n];
        const int r = segment_relation(p, far, q0, q1);
        if (r == 1 || (r == 0 && p.y == std::min(q0.y, q1.y) && q0.y != q1.y)) ++crossings;
    }
    return static_cast<int>(crossings & 1U);
}

Overlap rectangle_overlap(std::span<const Point, 4> quad, Point a, Point b) noexcept {
    const std::array<Point, 4> rect{Point{a.x, a.y}, Point{a.x, b.y}, Point{b.x, b.y}, Point{b.x, a.y}};
    std::vector<Point> found;
    for (const Point& corner : rect)
        if (inside_polygon(quad, corner) == 1) found.push_back(corner);
    Overlap out;
    if (found.size() >= 3) {
        out.any = true;
        out.corners = true;
        out.low = a;
        out.high = b;
        return out;
    }
    // The app keeps the points inside in four slots followed by those of the edge intersections, so a fifth and
    // sixth inside point share their slots with the first two intersections, which overwrite them.
    std::array<Point, 4 + 16> slots{};
    std::size_t inside = 0;
    for (const Point& corner : found) slots[inside++] = corner;
    for (const Point& q : quad)
        if (inside_polygon(rect, q) == 1) slots[inside++] = q;
    for (unsigned i = 0; i < 4; ++i) {
        const Point r0 = rect[i], r1 = rect[(i + 1) % 4];
        const std::int32_t rdx = sub(r1.x, r0.x), rdy = sub(r1.y, r0.y);
        for (unsigned k = 0; k < 4; ++k) {
            const Point q0 = quad[k], q1 = quad[(k + 1) % 4];
            if (segment_relation(r0, r1, q0, q1) == -1) continue;
            const std::int32_t qdx = sub(q1.x, q0.x), qdy = sub(q1.y, q0.y);
            if (mul(qdy, rdx) == mul(qdx, rdy)) continue;  // parallel (also keeps the divisions below nonzero)
            Point hit;
            if (i % 2 != 0) hit = {add(q0.x, divide(mul(qdx, sub(r0.y, q0.y)), qdy)), r0.y};  // horizontal edge
            else hit = {r0.x, add(q0.y, divide(mul(qdy, sub(r0.x, q0.x)), qdx))};          // vertical edge
            slots[4 + out.crossings++] = hit;
        }
    }
    found.assign(slots.begin(), slots.begin() + static_cast<std::ptrdiff_t>(inside));
    found.insert(found.end(), slots.begin() + 4, slots.begin() + 4 + out.crossings);
    if (found.empty()) return out;
    out.any = true;
    out.low = b;  // seeded crosswise, as the app does (the low output starts at the high corner)
    out.high = a;
    for (const Point& p : found) {
        out.low = {std::min(out.low.x, p.x), std::min(out.low.y, p.y)};
        out.high = {std::max(out.high.x, p.x), std::max(out.high.y, p.y)};
    }
    out.points = std::move(found);
    return out;
}

namespace {
constexpr unsigned overlap_capacity = 8;  // edge intersections the app has room for; more is left to its own code

ImportService orientation_service() {
    return [](ImportCall& call) -> Error {
        std::array<std::int32_t, 6> w{};
        if (!args(call, w)) return "orientation: unreadable argument";
        call.set_result(u(orientation({w[0], w[1]}, {w[2], w[3]}, {w[4], w[5]})));
        return std::nullopt;
    };
}
ImportService relation_service() {
    return [](ImportCall& call) -> Error {
        std::array<std::int32_t, 8> w{};
        if (!args(call, w)) return "segment_relation: unreadable argument";
        call.set_result(u(segment_relation({w[0], w[1]}, {w[2], w[3]}, {w[4], w[5]}, {w[6], w[7]})));
        return std::nullopt;
    };
}
ImportService inside_service() {
    return [](ImportCall& call) -> Error {
        const std::int32_t n = s(call.arg(1));
        if (n <= 0 || n > 65536) { call.decline(); return std::nullopt; }  // outside what was compared
        std::vector<std::int32_t> words(static_cast<std::size_t>(n) * 2);
        Point p;
        if (!load_words(call.runtime().memory, call.arg(0), words) || !load_point(call.runtime().memory, call.arg(2), p)) {
            call.decline();  // the guest's code reports the fault
            return std::nullopt;
        }
        std::vector<Point> points(static_cast<std::size_t>(n));
        for (std::size_t i = 0; i < points.size(); ++i) points[i] = {words[2 * i], words[2 * i + 1]};
        call.set_result(u(inside_polygon(points, p)));
        return std::nullopt;
    };
}
ImportService overlap_service() {
    return [](ImportCall& call) -> Error {
        auto& memory = call.runtime().memory;
        std::array<std::int32_t, 8> q{};
        Point a, b;
        std::uint32_t high_at = 0;
        if (!load_words(memory, call.arg(0) + 4, q) || !load_point(memory, call.arg(1), a) || !load_point(memory, call.arg(2), b) ||
            !call.read_arg(4, high_at)) {
            call.decline();
            return std::nullopt;
        }
        const std::array<Point, 4> quad{Point{q[0], q[1]}, Point{q[2], q[3]}, Point{q[4], q[5]}, Point{q[6], q[7]}};
        const Overlap o = rectangle_overlap(quad, a, b);
        if (o.crossings > overlap_capacity) { call.decline(); return std::nullopt; }
        if (o.any) {
            // Word by word through the pointers, in the app's order, so outputs that alias the inputs or each
            // other end up as they would.
            const std::uint32_t a_at = call.arg(1), b_at = call.arg(2), low_at = call.arg(3);
            bool ok = true;
            const auto ld = [&](std::uint32_t at) {
                std::array<std::int32_t, 1> w{};
                ok = ok && load_words(memory, at, w);
                return w[0];
            };
            const auto st = [&](std::uint32_t at, std::int32_t v) {
                std::array<std::byte, 4> bytes{};
                for (unsigned k = 0; k < 4; ++k) bytes[k] = std::byte((u(v) >> (8 * k)) & 255U);
                ok = ok && memory.write(at, bytes);
            };
            const std::uint32_t first_low = o.corners ? a_at : b_at, first_high = o.corners ? b_at : a_at;
            st(low_at, ld(first_low));
            st(low_at + 4, ld(first_low + 4));
            st(high_at, ld(first_high));
            st(high_at + 4, ld(first_high + 4));
            if (!o.corners)
                for (const Point& p : o.points) {
                    const std::int32_t lx = ld(low_at), ly = ld(low_at + 4);
                    st(low_at, std::min(lx, p.x));
                    st(low_at + 4, std::min(ly, p.y));
                    st(high_at, std::max(ld(high_at), p.x));
                    st(high_at + 4, std::max(ld(high_at + 4), p.y));
                }
            if (!ok) return "rectangle_overlap: outputs not writable";  // the guest's code would have faulted too
        }
        call.set_result(o.any ? 1U : 0U);
        return std::nullopt;
    };
}

// Coordinates that reach the degenerate cases (shared vertices, collinear and axis-parallel edges) as well as
// values whose products wrap.
std::int32_t coordinate(std::mt19937& random) {
    switch (random() % 8) {
    case 0: return s(random());
    case 1: case 2: return static_cast<std::int32_t>(random() % 2000001) - 1000000;
    default: return static_cast<std::int32_t>(random() % 13) - 6;
    }
}
Point point(std::mt19937& random) { return {coordinate(random), coordinate(random)}; }
}

NativeSet quad_geometry_set() {
    NativeSet set;
    set.name = "quad geometry";
    set.members.push_back({"orientation", {}, orientation_service(), true});
    set.members.push_back({"segment_relation", {}, relation_service(), true});
    set.members.push_back({"inside_polygon", {}, inside_service(), true});
    set.members.push_back({"rectangle_overlap", {}, overlap_service(), true});
    set.verify = [](NativeProbe& probe, const NativeSet::Call& guest, const NativeSet::Call& native) {
        std::mt19937 random(20261126);
        const auto same = [&](const char* name, const Words& arguments) {
            const auto g = guest(name, arguments), n = native(name, arguments);
            return g && n && g->r0 == n->r0;
        };
        for (unsigned i = 0; i < 3000; ++i) {
            const Point a = point(random), b = point(random), c = point(random), d = point(random);
            if (!same("orientation", {u(a.x), u(a.y), u(b.x), u(b.y), u(c.x), u(c.y)})) return false;
            if (!same("segment_relation", {u(a.x), u(a.y), u(b.x), u(b.y), u(c.x), u(c.y), u(d.x), u(d.y)})) return false;
        }
        Memory& memory = probe.memory();
        const std::uint32_t base = probe.scratch();
        const std::uint32_t object = base, a_at = base + 0x40, b_at = base + 0x48, low_at = base + 0x50, high_at = base + 0x58,
                            polygon = base + 0x100;
        for (unsigned i = 0; i < 1500; ++i) {
            const unsigned n = 1 + random() % 6;
            for (unsigned k = 0; k < n; ++k)
                if (!store_point(memory, polygon + 8 * k, point(random))) return false;
            if (!store_point(memory, a_at, point(random))) return false;
            if (!same("inside_polygon", {polygon, n, a_at})) return false;
        }
        for (unsigned i = 0; i < 2000; ++i) {
            std::array<Point, 4> quad{point(random), point(random), point(random), point(random)};
            const Point a = point(random), b = point(random);
            if (rectangle_overlap(quad, a, b).crossings > overlap_capacity) continue;  // left to the guest's code
            for (unsigned k = 0; k < 4; ++k)
                if (!store_point(memory, object + 4 + 8 * k, quad[k])) return false;
            if (!store_point(memory, a_at, a) || !store_point(memory, b_at, b)) return false;
            std::array<Point, 2> outputs[2];
            std::uint32_t results[2] = {};
            for (unsigned run = 0; run < 2; ++run) {
                if (!store_point(memory, low_at, {0x5a5a5a5a, 0x5a5a5a5a}) || !store_point(memory, high_at, {0x5a5a5a5a, 0x5a5a5a5a})) return false;
                const Words arguments{object, a_at, b_at, low_at, high_at};
                const auto r = run == 0 ? guest("rectangle_overlap", arguments) : native("rectangle_overlap", arguments);
                if (!r) return false;
                results[run] = r->r0;
                if (!load_point(memory, low_at, outputs[run][0]) || !load_point(memory, high_at, outputs[run][1])) return false;
            }
            if (results[0] != results[1] || outputs[0] != outputs[1]) return false;
        }
        return true;
    };
    return set;
}
}
