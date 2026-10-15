#pragma once
#include <array>

namespace orbitforge::core {
struct RelativityEvaluation {
    std::array<double, 3> schwarzschild_mps2, lense_thirring_mps2, de_sitter_mps2;
    std::array<double, 3> acceleration_mps2;
};
// IERS Conventions (2003), chapter 10, section 10.2 equation (1), beta=gamma=1.
// Earth-centred inertial satellite position [m] and velocity [m / TAI second].
// Geometric Sun position/velocity use the same ICRF-aligned axes, relative to
// Earth; velocities must be converted from ephemeris TDB to TAI rates explicitly.
// earth_pole is the unit ITRF +Z direction expressed in those inertial axes.
// Positive GM [m^3/s^2]; J_Earth/M_Earth=9.8e8 m^2/s, c=299792458 m/s.
// This leading post-Newtonian force model is not a full BCRS/GCRS coordinate
// transformation; multipole relativistic terms are excluded. TAI and TT rates
// coincide. Throws invalid_argument for nonfinite, degenerate or nonphysical inputs.
[[nodiscard]] RelativityEvaluation
evaluate_relativity(const std::array<double, 3> &position_m,
                    const std::array<double, 3> &velocity_mps, double earth_mu,
                    const std::array<double, 3> &sun_position_m,
                    const std::array<double, 3> &sun_velocity_mps, double sun_mu,
                    const std::array<double, 3> &earth_pole);
} // namespace orbitforge::core
