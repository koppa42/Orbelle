#pragma once
#include <array>
#include <orbitforge/core/frames/geodetic.hpp>
#include <orbitforge/core/time/time_provider.hpp>

namespace orbitforge::core {
enum class AtmosphereModel { jacchia_roberts, nrlmsise00 };
enum class LowAltitudeDensityModel { none, nrlmsise00 };
struct DragOptions {
    bool enabled = false;
    double cd = 2.2;
    double area_mass_ratio_m2_per_kg = .02;
    AtmosphereModel atmosphere = AtmosphereModel::jacchia_roberts;
    LowAltitudeDensityModel low_altitude_model = LowAltitudeDensityModel::none;
    double blending_range_m = 30000;
    // Manual constant fluxes in sfu; NRLMSISE uses the daily value for the previous day.
    double f107 = 150, f107_average = 150, kp = 3;
    // Geometric positions by default; apparent Sun requires a certifying ephemeris.
    bool use_apparent_sun_position = false;
    bool operator==(const DragOptions &) const = default;
};
void validate_drag_options(const DragOptions &options);
[[nodiscard]] DragOptions effective_drag_options(DragOptions options);
// Standard 1/3-Kp to Ap table; linear interpolation for intermediate Kp. [0,9].
[[nodiscard]] double kp_to_ap(double kp);
[[nodiscard]] GeodeticPosition drag_geodetic_position(const std::array<double, 3> &itrf_m);
struct AtmosphereInput {
    UtcDateTime utc{2000, 1, 1, 12, 0, 0, 0};
    GeodeticPosition geodetic{};
    // Angle east of the Sun meridian in Earth's equatorial plane. Local solar time=12+h*12/pi.
    double solar_hour_angle_rad = 0, sun_declination_rad = 0;
    double f107 = 150, f107_average = 150;
    double kp = 3;
};
struct AtmosphereEvaluation {
    double density_kg_m3, temperature_k, exospheric_temperature_k;
};
// Pure SI density model, no files or implicit time/ephemeris queries. JR: 90..2500 km;
// NRLMSISE-00 gtd7d (including anomalous oxygen): 0..1000 km. Invalid inputs throw.
[[nodiscard]] AtmosphereEvaluation evaluate_atmosphere(AtmosphereModel model,
                                                       const AtmosphereInput &input);
struct DragEvaluation {
    std::array<double, 3> acceleration_itrf_mps2{};
    double density_kg_m3 = 0;
    GeodeticPosition geodetic{};
};
// Satellite velocity is relative to the co-rotating atmosphere in ITRF. The caller
// must use a full inertial-to-ITRF state transform, including the rotation-rate term.
// Sun is geocentric in the same ITRF axes. No atmospheric winds or weather files implied.
[[nodiscard]] DragEvaluation evaluate_drag(const std::array<double, 3> &position_itrf_m,
                                           const std::array<double, 3> &velocity_itrf_mps,
                                           const std::array<double, 3> &sun_itrf_m,
                                           const UtcDateTime &utc, const DragOptions &options);
} // namespace orbitforge::core
