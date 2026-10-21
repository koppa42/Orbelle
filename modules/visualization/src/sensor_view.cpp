#include <algorithm>
#include <cmath>
#include <numbers>
#include <optional>
#include <orbitforge/visualization/sensor_view.hpp>
#include <stdexcept>

namespace orbitforge::visualization {
namespace {
using V = core::SensorVector;
using P = std::array<double, 2>;
constexpr double a = 6378137.0, b = 6356752.314245179;
constexpr double step = std::numbers::pi / 60;
double dot(V x, V y) { return x[0] * y[0] + x[1] * y[1] + x[2] * y[2]; }
V add(V x, V y) { return {x[0] + y[0], x[1] + y[1], x[2] + y[2]}; }
V mul(V x, double k) { return {x[0] * k, x[1] * k, x[2] * k}; }
V unit(V x) { return mul(x, 1 / std::sqrt(dot(x, x))); }
V scaled(V x) { return {x[0] / a, x[1] / a, x[2] / b}; }
V physical(V x) { return {x[0] * a, x[1] * a, x[2] * b}; }
double cross(P x, P y, P z) {
    return (y[0] - x[0]) * (z[1] - x[1]) - (y[1] - x[1]) * (z[0] - x[0]);
}
std::optional<V> intersect(V p, V direction) {
    const auto d = scaled(direction), q = scaled(p);
    const auto aa = dot(d, d), bb = dot(q, d), cc = dot(q, q) - 1;
    const auto disc = bb * bb - aa * cc;
    if (bb >= 0 || disc < 0)
        return {};
    // Stable near intersection, avoiding cancellation close to the surface.
    return add(p, mul(direction, cc / (-bb + std::sqrt(disc))));
}
// Subdivide surface facets for the longitude/latitude projection, especially near a pole.
void surface_triangle(std::vector<core::SensorTriangle> &out, V x, V y, V z, int depth = 0) {
    const auto qx = unit(scaled(x)), qy = unit(scaled(y)), qz = unit(scaled(z));
    if (depth < 7 && std::min({dot(qx, qy), dot(qy, qz), dot(qz, qx)}) < std::cos(step)) {
        const auto xy = physical(unit(add(qx, qy))), yz = physical(unit(add(qy, qz))),
                   zx = physical(unit(add(qz, qx)));
        surface_triangle(out, x, xy, zx, depth + 1);
        surface_triangle(out, xy, y, yz, depth + 1);
        surface_triangle(out, zx, yz, z, depth + 1);
        surface_triangle(out, xy, yz, zx, depth + 1);
    } else
        out.push_back({x, y, z});
}
} // namespace
SensorView::SensorView(core::SensorPattern pattern)
    : geometry_(std::move(pattern)), loops_(geometry_.boundaries(step)),
      triangles_(geometry_.triangles(step)) {}

SensorMesh SensorView::project(const V &position, const core::SensorAxes &axes,
                               double range) const {
    if (!std::isfinite(range) || range <= 0)
        throw std::invalid_argument("Sensor display range must be positive");
    const auto q = scaled(position);
    const auto r2 = dot(q, q);
    SensorMesh result;
    if (!std::isfinite(r2) || r2 <= 1 + 1e-10)
        return result;
    for (const auto &loop : loops_) {
        for (std::size_t i = 1; i < loop.size(); ++i) {
            const auto d0 = axes.to_world(loop[i - 1]), d1 = axes.to_world(loop[i]);
            const auto h0 = intersect(position, d0), h1 = intersect(position, d1);
            auto endpoint = [&](V d, const std::optional<V> &h) {
                return h ? *h : add(position, mul(d, range));
            };
            const auto e0 = endpoint(d0, h0), e1 = endpoint(d1, h1);
            result.sides.push_back({position, e0, e1});
            result.edges.push_back({e0, e1});
            // Four spokes per loop keep the volume legible without dense radial lines.
            if ((i - 1) % std::max<std::size_t>(1, (loop.size() - 1) / 4) == 0)
                result.edges.push_back({position, e0});
            if (h0 && h1)
                result.ground_edges.push_back({*h0, *h1});
        }
    }
    // In ellipsoid-scaled space the Earth is a unit sphere. Its apparent limb is
    // a circle on the gnomonic plane; clipping here handles a FOV enclosing Earth,
    // partial hits and holes without joining unrelated ray intersections.
    const auto basis = core::sensor_axes({1, 0, 0}, mul(q, -1));
    const auto radius = 1 / std::sqrt(r2 - 1);
    constexpr int count = 128;
    std::vector<P> disk;
    std::vector<V> limb;
    for (int i = 0; i < count; ++i) {
        const auto angle = 2 * std::numbers::pi * i / count;
        disk.push_back({radius * std::cos(angle), radius * std::sin(angle)});
        // Exact tangent point, avoiding round-off in a quadratic with zero discriminant.
        limb.push_back(physical(add(
            mul(q, 1 / r2), mul(add(mul(basis.x, std::cos(angle)), mul(basis.y, std::sin(angle))),
                                std::sqrt(1 - 1 / r2)))));
    }
    for (int i = 0; i < count; ++i) {
        const auto j = (i + 1) % count;
        const auto mid = physical(unit(add(scaled(limb[static_cast<std::size_t>(i)]),
                                           scaled(limb[static_cast<std::size_t>(j)]))));
        if (geometry_.margin(axes.to_local(add(mid, mul(position, -1)))) >= 0)
            result.ground_edges.push_back(
                {limb[static_cast<std::size_t>(i)], limb[static_cast<std::size_t>(j)]});
    }
    for (const auto &triangle : triangles_) {
        std::vector<V> front;
        for (const auto &v : triangle)
            front.push_back(unit(scaled(axes.to_world(v))));
        std::vector<V> clipped;
        for (std::size_t i = 0; i < front.size(); ++i) {
            const auto x = front[i], y = front[(i + 1) % front.size()];
            const auto dx = dot(x, basis.z) - 1e-8, dy = dot(y, basis.z) - 1e-8;
            if (dx >= 0)
                clipped.push_back(x);
            if ((dx >= 0) != (dy >= 0))
                clipped.push_back(add(x, mul(add(y, mul(x, -1)), dx / (dx - dy))));
        }
        std::vector<P> polygon;
        for (const auto &v : clipped)
            polygon.push_back(
                {dot(v, basis.x) / dot(v, basis.z), dot(v, basis.y) / dot(v, basis.z)});
        if (polygon.size() < 3)
            continue;
        // Reject polygons wholly outside the disk's bounding box before edge clipping.
        bool outside = false;
        for (std::size_t axis = 0; axis < 2; ++axis) {
            auto [lo, hi] = std::minmax_element(polygon.begin(), polygon.end(),
                                                [axis](P x, P y) { return x[axis] < y[axis]; });
            outside = outside || (*lo)[axis] > radius || (*hi)[axis] < -radius;
        }
        if (outside)
            continue;
        for (std::size_t i = 0; i < disk.size() && !polygon.empty(); ++i) {
            std::vector<P> next;
            const auto d0 = disk[i], d1 = disk[(i + 1) % disk.size()];
            for (std::size_t j = 0; j < polygon.size(); ++j) {
                const auto x = polygon[j], y = polygon[(j + 1) % polygon.size()];
                const auto dx = cross(d0, d1, x), dy = cross(d0, d1, y);
                if (dx >= 0)
                    next.push_back(x);
                if ((dx >= 0) != (dy >= 0)) {
                    const auto t = dx / (dx - dy);
                    next.push_back({x[0] + t * (y[0] - x[0]), x[1] + t * (y[1] - x[1])});
                }
            }
            polygon = std::move(next);
        }
        std::vector<V> hits;
        for (const auto &p : polygon) {
            const auto direction =
                physical(add(basis.z, add(mul(basis.x, p[0]), mul(basis.y, p[1]))));
            if (auto hit = intersect(position, direction))
                hits.push_back(*hit);
        }
        for (std::size_t i = 2; i < hits.size(); ++i)
            surface_triangle(result.ground, hits[0], hits[i - 1], hits[i]);
    }
    return result;
}
} // namespace orbitforge::visualization
