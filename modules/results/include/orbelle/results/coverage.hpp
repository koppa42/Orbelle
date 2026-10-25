#pragma once
#include <optional>
#include <orbelle/core/propagation/coverage.hpp>
#include <orbelle/core/propagation/region.hpp>
namespace orbelle::results {
struct CoveragePoint {
    core::CoverageCell cell;
    std::vector<core::AccessInterval> windows;
    core::CoverageTimeStatistics statistics;
};
// Immutable in a published report; all times/areas are SI. Areas are centre-point
// grid estimates, not the exact swept footprint of a sensor.
struct CoverageStatistics {
    core::CoverageGrid grid;
    std::optional<core::RegionBoundary> region;
    std::vector<CoveragePoint> points;
    double total_area_m2 = 0, ever_covered_area_m2 = 0;
    double mean_coverage_fraction = 0, maximum_gap_seconds = 0;
};
} // namespace orbelle::results
