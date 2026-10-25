#pragma once
#include <orbelle/core/propagation/coverage.hpp>

namespace orbelle::core {
struct RegionVertex {
    double longitude_rad = 0, latitude_rad = 0;
    bool operator==(const RegionVertex &) const = default;
};
// An empty vertex list denotes a latitude/longitude rectangle. Polygon edges are
// minor great-circle arcs in geodetic latitude/longitude; the simple polygon must
// fit within an open 180-degree longitude strip and exclude the poles. Vertices
// are not repeated at closure. All coordinates are radians on the WGS84 surface.
struct RegionBoundary {
    double south_rad = 20 * std::numbers::pi / 180;
    double north_rad = 50 * std::numbers::pi / 180;
    double west_rad = 90 * std::numbers::pi / 180;
    double east_rad = 130 * std::numbers::pi / 180;
    std::vector<RegionVertex> vertices;
    bool operator==(const RegionBoundary &) const = default;
};
void validate_region_boundary(const RegionBoundary &region);
// Retains the supplied sampling/constraint settings, replacing spatial bounds.
[[nodiscard]] CoverageGrid region_grid_bounds(const RegionBoundary &region,
                                              CoverageGrid settings = {});
// Streams retained cells only; rejected candidates are never stored.
void visit_region_coverage_grid(const RegionBoundary &region, const CoverageGrid &settings,
                                const std::function<void(const CoverageCell &)> &consume,
                                const std::function<void()> &check_cancelled = {});
// Centre-sampled cells inside the region; areas remain WGS84 cell estimates.
// Rejects an empty/coarse grid rather than inventing a covered sample.
[[nodiscard]] std::vector<CoverageCell> region_coverage_grid(const RegionBoundary &region,
                                                             CoverageGrid settings = {});
[[nodiscard]] bool region_contains(const RegionBoundary &region, RegionVertex point);
// Closed, continuously unwrapped longitude/latitude outline for display only.
[[nodiscard]] std::vector<RegionVertex> region_outline(const RegionBoundary &region);
} // namespace orbelle::core
