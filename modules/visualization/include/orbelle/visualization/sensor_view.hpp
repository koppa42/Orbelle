#pragma once
#include <cstdint>
#include <orbelle/core/propagation/sensor.hpp>

namespace orbelle::visualization {
struct SensorMesh {
    std::vector<core::SensorTriangle> sides;
    std::vector<core::SensorTriangle> ground;
    std::vector<std::array<core::SensorVector, 2>> edges, ground_edges;
    std::uint32_t color = 0xf1a6ca;
    double opacity = .16;
    bool fill = true;
};
// Cached angular tessellation; never used for Access calculations. Geometry in ITRF metres.
class SensorView {
  public:
    explicit SensorView(core::SensorPattern pattern);
    [[nodiscard]] SensorMesh project(const core::SensorVector &position,
                                     const core::SensorAxes &axes, double space_range_m) const;

  private:
    core::SensorGeometry geometry_;
    std::vector<std::vector<core::SensorVector>> loops_;
    std::vector<core::SensorTriangle> triangles_;
};
} // namespace orbelle::visualization
