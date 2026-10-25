#include <algorithm>
#include <array>
#include <cmath>
#include <orbelle/core/propagation/region.hpp>
#include <stdexcept>

namespace orbelle::core {
namespace {
constexpr double pi = std::numbers::pi, tolerance = 1e-12;
using Vector = std::array<double, 3>;
void check(bool ok, const char *message) {
    if (!ok)
        throw std::invalid_argument(message);
}
Vector cross(Vector a, Vector b) {
    return {a[1] * b[2] - a[2] * b[1], a[2] * b[0] - a[0] * b[2], a[0] * b[1] - a[1] * b[0]};
}
double dot(Vector a, Vector b) { return a[0] * b[0] + a[1] * b[1] + a[2] * b[2]; }
double norm(Vector a) { return std::sqrt(dot(a, a)); }
Vector unit(Vector a) {
    const auto n = norm(a);
    return {a[0] / n, a[1] / n, a[2] / n};
}
Vector direction(RegionVertex p) {
    return {std::cos(p.latitude_rad) * std::cos(p.longitude_rad),
            std::cos(p.latitude_rad) * std::sin(p.longitude_rad), std::sin(p.latitude_rad)};
}
double angle(Vector a, Vector b) { return std::atan2(norm(cross(a, b)), dot(a, b)); }
bool on_arc(Vector p, Vector a, Vector b) {
    return std::abs(angle(a, p) + angle(p, b) - angle(a, b)) <= tolerance;
}
double unwrap(double longitude, double reference) {
    return reference + std::remainder(longitude - reference, 2 * pi);
}
struct Polygon {
    std::vector<Vector> points;
    double west, east, south, north;
};
Polygon polygon(const RegionBoundary &r) {
    check(r.vertices.size() >= 3 && r.vertices.size() <= 256,
          "A region polygon requires 3 to 256 vertices");
    Polygon p{
        {}, r.vertices.front().longitude_rad, r.vertices.front().longitude_rad, pi / 2, -pi / 2};
    for (auto v : r.vertices) {
        check(std::isfinite(v.longitude_rad) && std::isfinite(v.latitude_rad) &&
                  v.longitude_rad >= -pi && v.longitude_rad <= pi && v.latitude_rad > -pi / 2 &&
                  v.latitude_rad < pi / 2,
              "Polygon vertices require finite longitude [-180,180] and latitude (-90,90)");
        const auto lon = unwrap(v.longitude_rad, r.vertices.front().longitude_rad);
        p.west = std::min(p.west, lon);
        p.east = std::max(p.east, lon);
        p.south = std::min(p.south, v.latitude_rad);
        p.north = std::max(p.north, v.latitude_rad);
        const auto a = direction(v);
        for (auto b : p.points)
            check(angle(a, b) > tolerance, "Region polygon has duplicate vertices");
        p.points.push_back(a);
    }
    check(p.east - p.west < pi - tolerance,
          "Region polygon longitude span must be less than 180 degrees");
    double signed_area = 0;
    for (std::size_t i = 0; i < p.points.size(); ++i) {
        const auto a = p.points[i], b = p.points[(i + 1) % p.points.size()];
        const auto n = cross(a, b);
        check(norm(n) > tolerance, "Region polygon has a degenerate edge");
        // Extremal latitude on the supporting great circle, if inside this edge.
        auto high = cross(n, cross(Vector{0, 0, 1}, n));
        if (norm(high) > tolerance) {
            high = unit(high);
            for (double sign : {1., -1.}) {
                const Vector v{sign * high[0], sign * high[1], sign * high[2]};
                if (on_arc(v, a, b)) {
                    const auto latitude = std::asin(std::clamp(v[2], -1., 1.));
                    p.south = std::min(p.south, latitude);
                    p.north = std::max(p.north, latitude);
                }
            }
        }
        if (i > 0 && i + 1 < p.points.size()) {
            const auto first = p.points.front();
            signed_area += 2 * std::atan2(dot(first, cross(a, b)),
                                          1 + dot(first, a) + dot(a, b) + dot(b, first));
        }
        for (std::size_t j = i + 1; j < p.points.size(); ++j) {
            const auto c = p.points[j], d = p.points[(j + 1) % p.points.size()];
            if (j == i + 1 || (i == 0 && j + 1 == p.points.size()))
                continue;
            auto x = cross(n, cross(c, d));
            if (norm(x) <= tolerance) {
                check(!on_arc(a, c, d) && !on_arc(b, c, d) && !on_arc(c, a, b) && !on_arc(d, a, b),
                      "Region polygon has overlapping edges");
                continue;
            }
            x = unit(x);
            for (double sign : {1., -1.}) {
                const Vector v{sign * x[0], sign * x[1], sign * x[2]};
                check(!(on_arc(v, a, b) && on_arc(v, c, d)), "Region polygon intersects itself");
            }
        }
    }
    check(std::abs(signed_area) > 1e-16 && p.north > p.south,
          "Region polygon must enclose a nonzero area");
    check(p.south > -pi / 2 + tolerance && p.north < pi / 2 - tolerance,
          "Region polygons crossing a pole are not supported; use latitude bounds");
    return p;
}
bool contains(const Polygon &p, RegionVertex v) {
    const auto lon = unwrap(v.longitude_rad, (p.west + p.east) / 2);
    if (lon < p.west - tolerance || lon > p.east + tolerance ||
        v.latitude_rad < p.south - tolerance || v.latitude_rad > p.north + tolerance)
        return false;
    const auto x = direction(v);
    double winding = 0;
    for (std::size_t i = 0; i < p.points.size(); ++i) {
        const auto a = p.points[i], b = p.points[(i + 1) % p.points.size()];
        if (on_arc(x, a, b))
            return true;
        winding += std::atan2(dot(x, cross(a, b)), dot(a, b) - dot(a, x) * dot(b, x));
    }
    return std::abs(winding) > pi;
}
CoverageGrid bounds(const RegionBoundary &r, CoverageGrid g, const Polygon *p = nullptr) {
    g.south_rad = p ? p->south : r.south_rad;
    g.north_rad = p ? p->north : r.north_rad;
    g.west_rad = p ? std::remainder(p->west, 2 * pi) : r.west_rad;
    g.east_rad = p ? std::remainder(p->east, 2 * pi) : r.east_rad;
    validate_coverage_grid(g);
    return g;
}
} // namespace
void validate_region_boundary(const RegionBoundary &r) {
    (void)bounds(r, {}); // Inactive rectangle drafts must also be valid.
    if (!r.vertices.empty())
        (void)polygon(r);
}
CoverageGrid region_grid_bounds(const RegionBoundary &r, CoverageGrid g) {
    validate_region_boundary(r);
    if (r.vertices.empty())
        return bounds(r, g);
    const auto p = polygon(r);
    return bounds(r, g, &p);
}
std::vector<CoverageCell> region_coverage_grid(const RegionBoundary &r, CoverageGrid g) {
    std::vector<CoverageCell> cells;
    const auto grid = region_grid_bounds(r, g);
    if (r.vertices.empty())
        return coverage_grid(grid);
    (void)coverage_grid_size(grid, cells.max_size());
    visit_region_coverage_grid(r, g, [&](const auto &cell) { cells.push_back(cell); });
    return cells;
}
void visit_region_coverage_grid(const RegionBoundary &r, const CoverageGrid &g,
                                const std::function<void(const CoverageCell &)> &consume,
                                const std::function<void()> &check_cancelled) {
    validate_region_boundary(r);
    check(bool(consume), "Coverage grid visitor is required");
    if (r.vertices.empty()) {
        visit_coverage_grid(bounds(r, g), consume, check_cancelled);
        return;
    }
    const auto p = polygon(r);
    bool any = false;
    visit_coverage_grid(
        bounds(r, g, &p),
        [&](const auto &cell) {
            if (contains(p, {cell.location.longitude_rad, cell.location.latitude_rad})) {
                any = true;
                consume(cell);
            }
        },
        check_cancelled);
    check(any, "No grid points lie in the region; reduce the grid spacing");
}
bool region_contains(const RegionBoundary &r, RegionVertex v) {
    validate_region_boundary(r);
    check(std::isfinite(v.longitude_rad) && std::isfinite(v.latitude_rad) &&
              std::abs(v.latitude_rad) <= pi / 2,
          "Region query coordinates must be finite and within latitude bounds");
    if (!r.vertices.empty())
        return contains(polygon(r), v);
    const auto g = bounds(r, {});
    auto longitude = std::fmod(v.longitude_rad - g.west_rad, 2 * pi);
    if (longitude < 0)
        longitude += 2 * pi;
    return v.latitude_rad >= g.south_rad && v.latitude_rad <= g.north_rad &&
           longitude <= coverage_longitude_span(g);
}
std::vector<RegionVertex> region_outline(const RegionBoundary &r) {
    const auto g = region_grid_bounds(r);
    std::vector<RegionVertex> vertices = r.vertices;
    if (vertices.empty())
        vertices = {{g.west_rad, g.south_rad},
                    {g.west_rad + coverage_longitude_span(g), g.south_rad},
                    {g.west_rad + coverage_longitude_span(g), g.north_rad},
                    {g.west_rad, g.north_rad}};
    std::vector<RegionVertex> result;
    for (std::size_t i = 0; i < vertices.size(); ++i) {
        const auto a = vertices[i], b = vertices[(i + 1) % vertices.size()];
        const auto va = direction(a), vb = direction(b);
        const auto length = r.vertices.empty()
                                ? std::max(std::abs(b.longitude_rad - a.longitude_rad),
                                           std::abs(b.latitude_rad - a.latitude_rad))
                                : angle(va, vb);
        const int count = std::max(1, int(std::ceil(length / (pi / 360))));
        for (int j = 0; j < count; ++j) {
            const auto t = double(j) / count;
            RegionVertex v{a.longitude_rad + t * (b.longitude_rad - a.longitude_rad),
                           a.latitude_rad + t * (b.latitude_rad - a.latitude_rad)};
            if (!r.vertices.empty()) {
                const auto s = std::sin(length);
                const auto x = std::sin((1 - t) * length) / s, y = std::sin(t * length) / s;
                const auto point = unit(
                    Vector{x * va[0] + y * vb[0], x * va[1] + y * vb[1], x * va[2] + y * vb[2]});
                v = {std::atan2(point[1], point[0]), std::asin(std::clamp(point[2], -1., 1.))};
                v.longitude_rad = unwrap(v.longitude_rad,
                                         result.empty() ? g.west_rad : result.back().longitude_rad);
            }
            result.push_back(v);
        }
    }
    result.push_back(result.front());
    return result;
}
} // namespace orbelle::core
