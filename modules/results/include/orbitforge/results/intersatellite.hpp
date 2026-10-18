#pragma once
#include <orbitforge/results/access_windows.hpp>

namespace orbitforge::results {
struct IntersatelliteStatistics {
    // Continuous global extrema over the query, including occulted times and endpoints.
    core::AccessExtremum minimum_range, maximum_range;
    AccessStatistics visibility;
    // Unformatted source intervals; templates cannot remove timeline information.
    std::vector<core::AccessInterval> windows;
};
} // namespace orbitforge::results
