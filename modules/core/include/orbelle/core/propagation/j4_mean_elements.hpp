#pragma once
#include <orbelle/core/propagation/central_body.hpp>
#include <orbelle/core/propagation/j2_mean_elements.hpp>

namespace orbelle::core {
// The same auxiliary mean-ellipse convention as J2; not osculating or TLE elements.
using J4MeanElements = J2MeanElements;
// EGM2008 (tide-free), unnormalized Jn = -sqrt(2n+1) Cn0.
[[nodiscard]] inline const CentralBody &j4_earth() {
    static constexpr CentralBody earth{
        OriginId::earth,       {3.986004415e14},      6378137.0,
        0.0010826261738522227, "earth-egm2008-j4-v1", -1.6198975999169731e-6};
    return earth;
}
} // namespace orbelle::core
