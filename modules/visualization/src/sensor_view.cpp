#include <algorithm>
#include <cmath>
#include <limits>
#include <numbers>
#include <optional>
#include <orbelle/visualization/sensor_view.hpp>
#include <stdexcept>

namespace orbelle::visualization {
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
// Intersect a convex angular polygon with the true apparent disk. Clipping to
// straight disk chords instead makes their intermediate vertices hit Earth
// before the limb, leaving inward notches between exact tangent vertices.
std::vector<P> clip_disk(const std::vector<P> &polygon, const std::vector<P> &disk, double radius) {
    const auto radius2 = radius * radius;
    const auto norm2 = [](P p) { return p[0] * p[0] + p[1] * p[1]; };
    if (std::all_of(polygon.begin(), polygon.end(), [&](P p) { return norm2(p) <= radius2; }))
        return polygon;
    std::vector<P> points;
    points.reserve(polygon.size() * 3 + disk.size());
    double winding = 0;
    for (std::size_t i = 0; i < polygon.size(); ++i) {
        const auto x = polygon[i], y = polygon[(i + 1) % polygon.size()];
        winding += x[0] * y[1] - x[1] * y[0];
        if (norm2(x) <= radius2)
            points.push_back(x);
        const P delta{y[0] - x[0], y[1] - x[1]};
        const auto aa = norm2(delta), bb = x[0] * delta[0] + x[1] * delta[1];
        const auto cc = norm2(x) - radius2, disc = bb * bb - aa * cc;
        if (aa == 0 || disc < 0)
            continue;
        const auto root = -bb - std::copysign(std::sqrt(disc), bb);
        const auto insert = [&](double t) {
            if (t >= 0 && t <= 1)
                points.push_back({x[0] + t * delta[0], x[1] + t * delta[1]});
        };
        insert(root / aa);
        if (root != 0)
            insert(cc / root);
    }
    // Clipping can collapse a triangle to a line. A zero winding must not
    // classify every disk point as inside that degenerate polygon.
    if (winding == 0)
        return {};
    for (const auto &p : disk) {
        bool inside = true;
        for (std::size_t i = 0; i < polygon.size(); ++i)
            if (cross(polygon[i], polygon[(i + 1) % polygon.size()], p) * winding < 0) {
                inside = false;
                break;
            }
        if (inside)
            points.push_back(p);
    }
    if (points.size() < 3)
        return {};
    P center{};
    for (const auto &p : points) {
        center[0] += p[0] / static_cast<double>(points.size());
        center[1] += p[1] / static_cast<double>(points.size());
    }
    std::sort(points.begin(), points.end(), [&](P x, P y) {
        return std::atan2(x[1] - center[1], x[0] - center[0]) <
               std::atan2(y[1] - center[1], y[0] - center[0]);
    });
    return points;
}
std::optional<V> intersect(V p, V direction) {
    const auto d = scaled(direction), q = scaled(p);
    const auto aa = dot(d, d), bb = dot(q, d), cc = dot(q, q) - 1;
    const auto disc = bb * bb - aa * cc;
    // Disk clipping produces exact tangent rays. Cancellation in the quadratic
    // may put their discriminant a few ulps below zero; keep those shared vertices.
    const auto tolerance =
        64 * std::numeric_limits<double>::epsilon() * (bb * bb + std::abs(aa * cc));
    if (bb >= 0 || disc < -tolerance)
        return {};
    // Stable near intersection, avoiding cancellation close to the surface.
    return add(p, mul(direction, cc / (-bb + std::sqrt(std::max(0., disc)))));
}
// In scaled space, an angular edge is a straight interpolation of two ray vectors.
// Its quadratic discriminant gives every limb crossing, including two crossings
// when both endpoints miss Earth. Roots on the backward cone are not Earth hits.
std::vector<double> limb_partitions(V q, V x, V y) {
    const auto delta = add(y, mul(x, -1));
    const auto cc = dot(q, q) - 1, qx = dot(q, x), qd = dot(q, delta);
    const auto aa = qd * qd - cc * dot(delta, delta);
    const auto bb = 2 * (qx * qd - cc * dot(x, delta));
    const auto c = qx * qx - cc * dot(x, x);
    std::vector<double> result{0, 1};
    const auto insert = [&](double t) {
        if (t > 0 && t < 1 && qx + t * qd < 0)
            result.push_back(t);
    };
    const auto epsilon = 64 * std::numeric_limits<double>::epsilon();
    if (std::abs(aa) <= epsilon * std::max(std::abs(bb), std::abs(c))) {
        if (bb != 0)
            insert(-c / bb);
    } else {
        const auto disc = bb * bb - 4 * aa * c;
        if (disc >= -epsilon * (bb * bb + std::abs(4 * aa * c))) {
            const auto root = -.5 * (bb + std::copysign(std::sqrt(std::max(0., disc)), bb));
            insert(root / aa);
            if (root != 0)
                insert(c / root);
        }
    }
    std::sort(result.begin(), result.end());
    result.erase(std::unique(result.begin(), result.end()), result.end());
    return result;
}
// Subdivide surface facets for the longitude/latitude projection, especially near a pole.
void surface_triangle(std::vector<core::SensorTriangle> &out, V x, V y, V z, int depth = 0) {
    const auto qx = unit(scaled(x)), qy = unit(scaled(y)), qz = unit(scaled(z));
    const std::array lengths{dot(qx, qy), dot(qy, qz), dot(qz, qx)};
    const auto longest = std::min_element(lengths.begin(), lengths.end());
    if (depth >= 18 || *longest >= std::cos(step)) {
        out.push_back({x, y, z});
        return;
    }
    // Split only the long edge. Splitting all three edges over-refines thin
    // radial facets and bends already-resolved limb edges away from their outline.
    if (longest == lengths.begin()) {
        const auto middle = physical(unit(add(qx, qy)));
        surface_triangle(out, x, middle, z, depth + 1);
        surface_triangle(out, middle, y, z, depth + 1);
    } else if (longest == lengths.begin() + 1) {
        const auto middle = physical(unit(add(qy, qz)));
        surface_triangle(out, x, y, middle, depth + 1);
        surface_triangle(out, x, middle, z, depth + 1);
    } else {
        const auto middle = physical(unit(add(qz, qx)));
        surface_triangle(out, x, y, middle, depth + 1);
        surface_triangle(out, middle, y, z, depth + 1);
    }
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
    const auto basis = core::sensor_axes({1, 0, 0}, mul(q, -1));
    constexpr int count = 128;
    std::vector<double> limb_angles;
    for (int i = 0; i < count; ++i)
        limb_angles.push_back(2 * std::numbers::pi * i / count);
    for (const auto &loop : loops_) {
        for (std::size_t i = 1; i < loop.size(); ++i) {
            const auto d0 = axes.to_world(loop[i - 1]), d1 = axes.to_world(loop[i]);
            const auto x = unit(scaled(d0)), y = unit(scaled(d1));
            const auto partitions = limb_partitions(q, x, y);
            const auto ray = [&](double t) {
                return unit(physical(add(mul(x, 1 - t), mul(y, t))));
            };
            for (std::size_t j = 1; j + 1 < partitions.size(); ++j) {
                const auto direction = scaled(ray(partitions[j]));
                auto angle = std::atan2(dot(direction, basis.y), dot(direction, basis.x));
                if (angle < 0)
                    angle += 2 * std::numbers::pi;
                limb_angles.push_back(angle);
            }
            for (std::size_t j = 1; j < partitions.size(); ++j) {
                const auto first = ray(partitions[j - 1]), last = ray(partitions[j]);
                const auto hits = intersect(position, ray((partitions[j - 1] + partitions[j]) / 2));
                const auto h0 = hits ? intersect(position, first) : std::nullopt;
                const auto h1 = hits ? intersect(position, last) : std::nullopt;
                // Keep separate endpoints on either side of a tangent ray: its
                // ground endpoint and the adjacent space extension are not one rim.
                const auto e0 = h0 ? *h0 : add(position, mul(first, range));
                const auto e1 = h1 ? *h1 : add(position, mul(last, range));
                (hits ? result.sides : result.space_sides).push_back({position, e0, e1});
                auto &edges = hits ? result.edges : result.space_edges;
                if (!hits)
                    edges.push_back({e0, e1});
                // Four regular spokes plus the edges exposed by limb clipping.
                if (j > 1 || (i - 1) % std::max<std::size_t>(1, (loop.size() - 1) / 4) == 0)
                    edges.push_back({position, e0});
                if (j + 1 < partitions.size())
                    edges.push_back({position, e1});
                if (h0 && h1)
                    result.ground_edges.push_back({*h0, *h1});
            }
        }
    }
    // In ellipsoid-scaled space the Earth is a unit sphere. Its apparent limb is
    // a circle on the gnomonic plane; clipping here handles a FOV enclosing Earth,
    // partial hits and holes without joining unrelated ray intersections.
    const auto radius = 1 / std::sqrt(r2 - 1);
    std::vector<P> disk;
    std::sort(limb_angles.begin(), limb_angles.end());
    limb_angles.erase(std::unique(limb_angles.begin(), limb_angles.end()), limb_angles.end());
    const auto tangent_point = [&](double angle) {
        return physical(add(mul(q, 1 / r2),
                            mul(add(mul(basis.x, std::cos(angle)), mul(basis.y, std::sin(angle))),
                                std::sqrt(1 - 1 / r2))));
    };
    for (const auto angle : limb_angles)
        disk.push_back({radius * std::cos(angle), radius * std::sin(angle)});
    for (std::size_t i = 0; i < limb_angles.size(); ++i) {
        const auto first = limb_angles[i];
        const auto last = i + 1 < limb_angles.size() ? limb_angles[i + 1] : 2 * std::numbers::pi;
        const auto mid = tangent_point((first + last) / 2);
        if (geometry_.margin(axes.to_local(add(mid, mul(position, -1)))) >= 0) {
            const auto start = tangent_point(first), end = tangent_point(last);
            result.ground_edges.push_back({start, end});
            // A coarse tangent fan cuts into Earth between its rim vertices.
            // Keep each rim chord within the renderer's small surface lift.
            const auto segments =
                std::max(1, static_cast<int>(std::ceil((last - first) / (std::numbers::pi / 360))));
            auto previous = start;
            for (int j = 1; j <= segments; ++j) {
                const auto next = tangent_point(first + (last - first) * j / segments);
                result.limb_sides.push_back({position, previous, next});
                previous = next;
            }
        }
    }
    for (const auto &triangle : triangles_) {
        std::vector<V> clipped;
        for (const auto &v : triangle)
            clipped.push_back(unit(scaled(axes.to_world(v))));
        // Clip in homogeneous ray coordinates before the perspective divide.
        // A front-plane epsilon alone creates enormous coordinates for oblique
        // FOVs; subtracting those during disk clipping loses tangent vertices.
        for (const auto &normal :
             {add(mul(basis.z, radius), basis.x), add(mul(basis.z, radius), mul(basis.x, -1)),
              add(mul(basis.z, radius), basis.y), add(mul(basis.z, radius), mul(basis.y, -1))}) {
            std::vector<V> next;
            for (std::size_t i = 0; i < clipped.size(); ++i) {
                const auto x = clipped[i], y = clipped[(i + 1) % clipped.size()];
                const auto dx = dot(x, normal), dy = dot(y, normal);
                if (dx >= 0)
                    next.push_back(x);
                if ((dx >= 0) != (dy >= 0))
                    next.push_back(add(x, mul(add(y, mul(x, -1)), dx / (dx - dy))));
            }
            clipped = std::move(next);
        }
        std::vector<P> polygon;
        for (const auto &v : clipped)
            if (dot(v, basis.z) > 0)
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
        polygon = clip_disk(polygon, disk, radius);
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
} // namespace orbelle::visualization
