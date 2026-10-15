#pragma once
#include <orbitforge/core/propagation/gravity.hpp>
#include <span>

namespace orbitforge::core {
enum class SolidTideMode { none, permanent, full };
struct SolidTideOptions {
    SolidTideMode mode = SolidTideMode::none;
    bool truncate_to_gravity = true;
    bool operator==(const SolidTideOptions &) const = default;
};
struct TideRaisingBody {
    std::array<double, 3> position_itrf_m{}; // Geometric, Earth-centred, same epoch as the field.
    double mu =
        0; // m^3/s^2, from the ephemeris solution, independent of direct third-body options.
};
void validate_solid_tide_options(const SolidTideOptions &options);
// Canonical calculation settings; inactive drafts remain in the saved options.
[[nodiscard]] SolidTideOptions effective_solid_tide_options(SolidTideOptions options,
                                                            int gravity_degree);
// Fully normalized IERS 2003, anelastic Earth, k20 = 0.30190 (post-2006 STK default).
// Full includes the instantaneous Sun/Moon deformation (degrees 2/3, induced degree 4).
// Frequency-dependent constituents and pole tide are not yet included, corresponding
// to STK's Full with Include Time Dependent Terms off. No constituent amplitude filter.
// None removes the separable permanent deformation from a zero-tide base field;
// Permanent includes it exactly once; Full already contains it in the body response.
// Only tide_free / zero_tide fields are supported; mean_tide is rejected explicitly.
// Input/output axes are ITRF; no downloads, frame conversion or ephemeris resolution.
// Full requires exactly two bodies (Sun and Moon); other modes do not read bodies.
// Invalid options/metadata/geometry throw std::invalid_argument.
[[nodiscard]] std::vector<GravityCoefficient>
solid_tide_corrections(const GravityInfo &field, const SolidTideOptions &options,
                       int gravity_degree, int gravity_order,
                       std::span<const TideRaisingBody> bodies = {});
[[nodiscard]] double iers2003_permanent_tide_c20() noexcept;
} // namespace orbitforge::core
