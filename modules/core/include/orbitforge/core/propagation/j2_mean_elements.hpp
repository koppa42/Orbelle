#pragma once
#include <orbitforge/core/time/epoch.hpp>

namespace orbitforge::core {
// First-order secular J2 mean ellipse, referred to a fixed GCRF equatorial plane.
// This is NOT an osculating state or a Brouwer-Lyddane/SGP4 mean element set.
// SI, radians; closed ellipses outside the central body's reference radius only.
struct J2MeanElements {
    Epoch epoch;
    double semi_major_axis_m;
    double eccentricity;
    double inclination_rad;
    double raan_rad;
    double argument_of_periapsis_rad;
    double mean_anomaly_rad;
};
struct J2SecularOptions {
    bool node_precession = true;
    bool apsidal_precession = true;
    bool mean_motion_correction = true;
    bool operator==(const J2SecularOptions &) const = default;
};
} // namespace orbitforge::core
