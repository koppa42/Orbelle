#pragma once
#include <array>
#include <orbelle/core/propagation/region.hpp>
#include <vector>

namespace orbelle::visualization {
struct RegionMesh {
    using Position = std::array<double, 3>;
    std::vector<Position> outline;
    std::vector<std::array<Position, 3>> triangles;
};
// Cached display geometry in Earth-centred ITRF metres, lifted 1 km above WGS84.
// Rectangle edges follow latitude/longitude; polygon edges are the same minor
// great-circle arcs as RegionBoundary. Never used for coverage or area analysis.
[[nodiscard]] RegionMesh region_mesh(const core::RegionBoundary &, bool fill = true);
} // namespace orbelle::visualization
