#pragma once
#include <array>
#include <optional>

namespace orbitforge::core {
enum class SrpShadowModel { none, cylindrical, dual_cone };
struct SrpOptions {
    bool enabled = false;
    double cr = 1;
    double area_mass_ratio_m2_per_kg = 0.02;
    SrpShadowModel shadow_model = SrpShadowModel::dual_cone;
    bool earth_shadow = true, moon_shadow = true;
    double atmospheric_altitude_m = 0;
    double pressure_at_reference_pa = 4.56e-6;
    double reference_distance_m = 149597870700.0;
    double sun_radius_m = 695700000, earth_radius_m = 6378137, moon_radius_m = 1737400;
    bool operator==(const SrpOptions &) const = default;
};
// Throws invalid_argument for invalid enums, nonfinite values, Cr outside [-100,100],
// A/m outside [0,9999] m²/kg, nonpositive constants or negative atmospheric altitude.
// Disabled settings are also validated so persistent drafts remain well defined.
void validate_srp_options(const SrpOptions &options);
// Normalizes inactive drafts for calculation identity; does not change saved settings.
[[nodiscard]] SrpOptions effective_srp_options(SrpOptions options);
struct SrpEvaluation {
    std::array<double, 3> acceleration_mps2{};
    double lighting_ratio = 1;
};
// SI, geocentric geometric positions in the same inertial axes at one TAI epoch.
// Spherical satellite and occultors. Dual cone uses the uniform apparent solar disk
// and the union of Earth's/Moon's apparent disks, including annular eclipses.
// Throws invalid_argument for missing required Moon geometry; PropagationError for
// nonfinite positions/force or an observer inside the Sun/assigned occultor.
// Moon is required only with SRP and a non-none shadow model assigning the Moon.
// Disabled SRP does not inspect positions. No light-time/aberration,
// ellipsoidal limb, atmospheric refraction or STK boundary mitigation is implied.
[[nodiscard]] SrpEvaluation evaluate_srp(const std::array<double, 3> &satellite_m,
                                         const std::array<double, 3> &sun_m,
                                         const SrpOptions &options,
                                         const std::optional<std::array<double, 3>> &moon_m = {});
} // namespace orbitforge::core
