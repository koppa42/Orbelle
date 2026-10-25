#pragma once
#include <functional>
#include <limits>
#include <numbers>
#include <orbelle/core/propagation/access.hpp>

namespace orbelle::core {
// WGS84 surface grid, geodetic radians. West > east crosses the date line;
// [-pi,+pi] covers all longitudes. Cells use latitude/longitude boundaries,
// never great-circle polygon edges. Longitude spacing widens toward the poles.
struct CoverageGrid {
    double south_rad = 20 * std::numbers::pi / 180;
    double north_rad = 50 * std::numbers::pi / 180;
    double west_rad = 90 * std::numbers::pi / 180;
    double east_rad = 130 * std::numbers::pi / 180;
    double resolution_rad = 5 * std::numbers::pi / 180;
    double minimum_elevation_rad = 0;
    bool include_end_gaps = true;
    bool operator==(const CoverageGrid &) const = default;
};
struct CoverageCell {
    GroundStationLocation location;
    // Longitude bounds are continuous eastward from grid.west_rad, possibly > pi.
    double south_rad, north_rad, west_rad, east_rad, area_m2;
};
void validate_coverage_grid(const CoverageGrid &grid);
[[nodiscard]] double coverage_longitude_span(const CoverageGrid &grid);
// No fixed point-count limit. The visitor owns storage and may throw to cancel.
// Size queries allocate no cells; maximum_cells is a caller's resource budget.
[[nodiscard]] std::size_t
coverage_grid_size(const CoverageGrid &grid,
                   std::size_t maximum_cells = std::numeric_limits<std::size_t>::max(),
                   const std::function<void()> &check_cancelled = {});
void visit_coverage_grid(const CoverageGrid &grid,
                         const std::function<void(const CoverageCell &)> &consume,
                         const std::function<void()> &check_cancelled = {});
// Materializing convenience API, subject to vector addressability/allocation.
[[nodiscard]] std::vector<CoverageCell> coverage_grid(const CoverageGrid &grid);
// Union touching/overlapping positive intervals within [first,last]. Inputs must
// already be clipped to the analysis interval. Does not fill sub-tolerance gaps.
[[nodiscard]] std::vector<AccessInterval>
merge_coverage_intervals(Epoch first, Epoch last, std::vector<AccessInterval> intervals);
struct CoverageTimeStatistics {
    double covered_seconds = 0, maximum_gap_seconds = 0, mean_gap_seconds = 0;
    std::size_t gap_count = 0;
};
// Revisit = uncovered time, not start-to-start period. Optional end gaps use the
// analysis bounds (optimistic/clipped). No access => full interval, even if ends
// are ignored; continuous coverage => zero. Requires a positive analysis interval.
[[nodiscard]] CoverageTimeStatistics
coverage_time_statistics(Epoch first, Epoch last, const std::vector<AccessInterval> &merged,
                         bool include_end_gaps);
} // namespace orbelle::core
