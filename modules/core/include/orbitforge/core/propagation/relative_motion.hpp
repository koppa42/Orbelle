#pragma once
#include <optional>
#include <orbitforge/core/frames/state.hpp>

namespace orbitforge::core {
struct RelativeMotion {
    double range_m, relative_speed_mps;
    // Inertial dot(delta position, delta velocity) / range; undefined at coincidence.
    std::optional<double> range_rate_mps;
    // Target minus primary, projected on primary's radial/in-track/cross-track axes.
    std::array<double, 3> ric_position_m;
};
// Same-epoch Earth/GCRF states in SI. RIC is undefined for radial/zero-speed primaries.
// No light time, rotating-frame velocity, collision probability or sampled derivatives.
[[nodiscard]] RelativeMotion relative_motion(const CartesianState &primary,
                                             const CartesianState &target);
// Same-epoch Earth/ITRF states. Minimum squared ellipsoid-scaled radius on the
// connecting segment minus one; >= 0 is unobstructed (tangent included).
// Fixed WGS84 ellipsoid, no terrain, refraction, atmosphere or light time.
[[nodiscard]] double intersatellite_clearance(const CartesianState &primary,
                                              const CartesianState &target);
} // namespace orbitforge::core
