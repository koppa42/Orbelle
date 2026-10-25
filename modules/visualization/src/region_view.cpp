#include <algorithm>
#include <cmath>
#include <numeric>
#include <orbelle/visualization/region_view.hpp>
#include <stdexcept>

namespace orbelle::visualization {
namespace {
using V = RegionMesh::Position;
constexpr double pi = std::numbers::pi;
V direction(core::RegionVertex p) {
    return {std::cos(p.latitude_rad) * std::cos(p.longitude_rad),
            std::cos(p.latitude_rad) * std::sin(p.longitude_rad), std::sin(p.latitude_rad)};
}
double dot(V a, V b) { return a[0] * b[0] + a[1] * b[1] + a[2] * b[2]; }
V middle(V a, V b) {
    V p{a[0] + b[0], a[1] + b[1], a[2] + b[2]};
    const auto length = std::hypot(p[0], p[1], p[2]);
    for (auto &v : p)
        v /= length;
    return p;
}
V surface(core::RegionVertex p) {
    // Display outlines deliberately unwrap date-line crossings for 2D; the
    // geodetic conversion requires a longitude in [-pi, pi].
    return core::ground_station_position(
        {std::remainder(p.longitude_rad, 2 * pi), p.latitude_rad, 1000, 0});
}
V surface(V p) {
    return surface(
        core::RegionVertex{std::atan2(p[1], p[0]), std::atan2(p[2], std::hypot(p[0], p[1]))});
}
void triangle(RegionMesh &mesh, V a, V b, V c) {
    // Split the longest arc: unlike a triangle fan, this preserves concave
    // boundaries and keeps the chord above the ellipsoid everywhere.
    const std::array cosine{dot(a, b), dot(b, c), dot(c, a)};
    const auto edge = std::min_element(cosine.begin(), cosine.end()) - cosine.begin();
    if (cosine[edge] >= std::cos(pi / 180)) {
        mesh.triangles.push_back({surface(a), surface(b), surface(c)});
    } else if (edge == 0) {
        const auto m = middle(a, b);
        triangle(mesh, a, m, c);
        triangle(mesh, m, b, c);
    } else if (edge == 1) {
        const auto m = middle(b, c);
        triangle(mesh, a, b, m);
        triangle(mesh, a, m, c);
    } else {
        const auto m = middle(c, a);
        triangle(mesh, a, b, m);
        triangle(mesh, m, b, c);
    }
}
using P = std::array<long double, 2>;
long double turn(P a, P b, P c) {
    return (b[0] - a[0]) * (c[1] - a[1]) - (b[1] - a[1]) * (c[0] - a[0]);
}
} // namespace
RegionMesh region_mesh(const core::RegionBoundary &boundary, bool fill) {
    RegionMesh mesh;
    for (const auto &p : core::region_outline(boundary))
        mesh.outline.push_back(surface(p));
    if (!fill)
        return mesh;
    const auto bounds = core::region_grid_bounds(boundary);
    const auto span = core::coverage_longitude_span(bounds);
    if (boundary.vertices.empty()) {
        // Follow parallels exactly at the mesh vertices, including full-world
        // rectangles and pole caps. A one-degree cell diagonal sags < 500 m.
        const int rows =
            std::max(1, int(std::ceil((bounds.north_rad - bounds.south_rad) / (pi / 180))));
        const int columns = std::max(1, int(std::ceil(span / (pi / 180))));
        const auto point = [&](int x, int y) {
            return surface(core::RegionVertex{
                bounds.west_rad + span * double(x) / columns,
                bounds.south_rad + (bounds.north_rad - bounds.south_rad) * double(y) / rows});
        };
        for (int y = 0; y < rows; ++y)
            for (int x = 0; x < columns; ++x) {
                const auto a = point(x, y), b = point(x + 1, y), c = point(x + 1, y + 1),
                           d = point(x, y + 1);
                mesh.triangles.push_back({a, b, c});
                mesh.triangles.push_back({a, c, d});
            }
        return mesh;
    }
    // The supported longitude strip is narrower than 180 degrees, hence lies
    // in this open hemisphere. Gnomonic projection makes each great-circle
    // edge straight, allowing ordinary ear clipping without filling concavities.
    const long double centre = bounds.west_rad + span / 2;
    std::vector<P> points;
    std::vector<V> directions;
    for (const auto &p : boundary.vertices) {
        const auto longitude =
            std::remainder(static_cast<long double>(p.longitude_rad) - centre, 2 * pi);
        points.push_back({std::tan(longitude), std::tan(static_cast<long double>(p.latitude_rad)) /
                                                   std::cos(longitude)});
        directions.push_back(direction(p));
    }
    long double area = 0;
    for (std::size_t i = 0; i < points.size(); ++i)
        area += turn(points.front(), points[i], points[(i + 1) % points.size()]);
    const long double sign = area > 0 ? 1 : -1;
    std::vector<std::size_t> remaining(points.size());
    std::iota(remaining.begin(), remaining.end(), 0);
    while (remaining.size() > 3) {
        bool found = false;
        for (std::size_t i = 0; i < remaining.size(); ++i) {
            const auto a = remaining[(i + remaining.size() - 1) % remaining.size()],
                       b = remaining[i], c = remaining[(i + 1) % remaining.size()];
            if (sign * turn(points[a], points[b], points[c]) <= 0)
                continue;
            bool occupied = false;
            for (auto k : remaining)
                if (k != a && k != b && k != c &&
                    sign * turn(points[a], points[b], points[k]) >= 0 &&
                    sign * turn(points[b], points[c], points[k]) >= 0 &&
                    sign * turn(points[c], points[a], points[k]) >= 0) {
                    occupied = true;
                    break;
                }
            if (occupied)
                continue;
            triangle(mesh, directions[a], directions[b], directions[c]);
            remaining.erase(remaining.begin() + static_cast<std::ptrdiff_t>(i));
            found = true;
            break;
        }
        if (!found)
            throw std::invalid_argument("Region display boundary cannot be triangulated");
    }
    triangle(mesh, directions[remaining[0]], directions[remaining[1]], directions[remaining[2]]);
    return mesh;
}
} // namespace orbelle::visualization
