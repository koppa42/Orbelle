#pragma once
#include <orbelle/core/time/epoch.hpp>

namespace orbelle::core {
// Osculating elements in Earth-centred GCRF, SI lengths and radians.
// Elliptic: a > 0, 0 <= e < 1. Hyperbolic: a < 0, e > 1.
// Parabolic states have no finite semi-major axis; use CartesianState instead.
// Circular/equatorial orbits use the provider's conventional zero singular angles.
struct KeplerianElements {
    Epoch epoch;
    double semi_major_axis_m;
    double eccentricity;
    double inclination_rad;
    double raan_rad;
    double argument_of_periapsis_rad;
    double true_anomaly_rad;
};
} // namespace orbelle::core
