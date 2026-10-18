#pragma once
#include <orbitforge/core/propagation/coverage.hpp>
namespace orbitforge::results {
struct CoveragePoint {
    core::CoverageCell cell;
    std::vector<core::AccessInterval> windows;
    core::CoverageTimeStatistics statistics;
};
// Immutable in a published report; all times/areas are SI. Areas are centre-point
// grid estimates, not the exact swept footprint of a sensor.
struct CoverageStatistics {
    std::vector<CoveragePoint> points;
    double total_area_m2 = 0, ever_covered_area_m2 = 0;
    double mean_coverage_fraction = 0, maximum_gap_seconds = 0;
};
} // namespace orbitforge::results
