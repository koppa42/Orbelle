#pragma once
#include <orbelle/core/propagation/atmospheric_drag.hpp>

namespace orbelle::core::detail {
// Force-policy cutoff, independent of empirical model validity and the integrator stop.
inline constexpr double drag_cutoff_altitude_m = 2500000;
double drag_density_altitude(const std::array<double, 3> &itrf_m, const GeodeticPosition &geodetic,
                             bool approximate);
AtmosphereEvaluation jacchia_roberts(const AtmosphereInput &input, double utc_gmat_mjd);
AtmosphereEvaluation nrlmsise00(const AtmosphereInput &input, int doy, double seconds_in_day);
} // namespace orbelle::core::detail
