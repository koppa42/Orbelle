#pragma once
#include <orbelle/core/propagation/keplerian_elements.hpp>
#include <stdexcept>

namespace orbelle::core {
enum class EarthOrbitType {
    circular,
    critically_inclined,
    critically_inclined_sun_sync,
    geosynchronous,
    molniya,
    orbit_designer,
    repeating_ground_trace,
    repeating_sun_sync,
    sun_synchronous
};
// Earth-only preliminary design, SI and radians in the fixed GCRF equatorial plane.
// Heights use central_body(Earth).equatorial_radius_m. Inactive fields are ignored.
// Repetition: integer nodal revolutions / integer rotations relative to the node;
// a nodal day is 2*pi/(earth_rotation_rate - RAAN_rate), not necessarily 86400 s.
struct EarthOrbitDesignRequest {
    EarthOrbitType type = EarthOrbitType::circular;
    Epoch epoch{0};
    double altitude_m = 500000;
    double perigee_altitude_m = 500000;
    double apogee_altitude_m = 40000000;
    double semi_major_axis_m = 7628136.6;
    double eccentricity = 0.05;
    double inclination_rad = 0.9250245035569946; // 53 degrees
    double raan_rad = 0;
    double argument_of_periapsis_rad = 0;
    double true_anomaly_rad = 0; // Circular designs: argument of latitude.
    bool retrograde = false;     // Critically inclined only.
    bool sun_sync_by_inclination = false;
    int repeat_revolutions = 15;
    int repeat_days = 1;
};
enum class OrbitDesignErrorCode { invalid_parameter, below_surface, no_solution, repeat_ratio };
class OrbitDesignError : public std::invalid_argument {
  public:
    OrbitDesignError(OrbitDesignErrorCode code, const char *message)
        : std::invalid_argument(message), code(code) {}
    OrbitDesignErrorCode code;
};
struct EarthOrbitDesign {
    // Auxiliary geometric ellipse when secular_j2=true, NOT osculating elements.
    KeplerianElements elements;
    bool secular_j2;
    double nodal_period_seconds;
    double repeat_cycle_seconds; // Zero for designs without a repeat constraint.
    double raan_rate_rad_s;
    double periapsis_rate_rad_s;
};
// IERS nominal spin and nominal tropical-year mean Sun rate. These constants
// define design constraints, not a time-dependent Earth orientation ephemeris.
inline constexpr double design_earth_rotation_rad_s = 7.2921150e-5;
inline constexpr double design_sun_rate_rad_s = 1.991063797294792e-7; // 365.2422 SI days
// Bounded, deterministic solves; throws OrbitDesignError for invalid/unreachable inputs.
// No atmosphere, third bodies, short-period corrections or station keeping.
[[nodiscard]] EarthOrbitDesign design_earth_orbit(const EarthOrbitDesignRequest &request);
} // namespace orbelle::core
