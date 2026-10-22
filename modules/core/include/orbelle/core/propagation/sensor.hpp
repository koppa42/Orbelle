#pragma once
#include <array>
#include <string_view>
#include <vector>

namespace orbelle::core {
enum class SensorPatternType { simple_conic, complex_conic, rectangular, custom };
struct SensorPatternPoint {
    double cone_rad = 0, clock_rad = 0;
    bool operator==(const SensorPatternPoint &) const = default;
};
// Angles in radians. +Z is boresight, clock runs from +X towards +Y.
// Custom boundaries are simple polygons in the forward hemisphere; edges are minor
// great-circle arcs, shared by analysis and visualization. No implicit file dependency.
struct SensorPattern {
    SensorPatternType type = SensorPatternType::simple_conic;
    double half_angle_rad = 0.7853981633974483;
    double inner_half_angle_rad = 0;
    double minimum_clock_rad = 0, maximum_clock_rad = 6.283185307179586;
    double horizontal_half_angle_rad = 0.5235987755982988;
    double vertical_half_angle_rad = 0.3490658503988659;
    std::vector<SensorPatternPoint> custom_points;
    SensorPattern() = default;
    SensorPattern(double half_angle) : half_angle_rad(half_angle) {}
    bool operator==(const SensorPattern &) const = default;
};
using SensorVector = std::array<double, 3>;
using SensorTriangle = std::array<SensorVector, 3>;
// Validate once and retain immutable geometry for repeated visibility queries.
class SensorGeometry {
  public:
    explicit SensorGeometry(SensorPattern pattern);
    [[nodiscard]] const SensorPattern &pattern() const { return pattern_; }
    // Signed angular clearance in radians; zero is on the included boundary.
    [[nodiscard]] double margin(const SensorVector &local_direction) const;
    // Closed boundary loops and a tessellated angular domain, for display only.
    [[nodiscard]] std::vector<std::vector<SensorVector>> boundaries(double step_rad) const;
    [[nodiscard]] std::vector<SensorTriangle> triangles(double step_rad) const;

  private:
    SensorPattern pattern_;
    std::vector<SensorVector> vertices_;
    std::vector<std::array<double, 2>> plane_;
    std::vector<std::array<std::size_t, 3>> triangles_;
};
struct SensorAxes {
    SensorVector x, y, z;
    [[nodiscard]] SensorVector to_local(const SensorVector &direction) const;
    [[nodiscard]] SensorVector to_world(const SensorVector &direction) const;
};
// X is the ITRF velocity projected perpendicular to boresight; degenerate velocity
// uses the least aligned ITRF coordinate axis. Positive roll rotates X toward Y.
[[nodiscard]] SensorAxes sensor_axes(const SensorVector &velocity_itrf,
                                     const SensorVector &boresight_itrf, double roll_rad = 0);
// STK .pattern subset: HalfAngleAzimuthData, AzElMaskData, ReferenceDistance/PatternData.
// Unknown directives, open/self-intersecting contours, and rear-hemisphere points fail.
[[nodiscard]] SensorPattern parse_sensor_pattern(std::string_view text);
} // namespace orbelle::core
