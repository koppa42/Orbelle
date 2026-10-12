#pragma once
#include <orbitforge/core/frames/state.hpp>
#include <orbitforge/core/units/quantities.hpp>
#include <stdexcept>
#include <string_view>

namespace orbitforge::core {
struct CentralBody {
    OriginId origin;
    GravitationalParameter mu;
    double equatorial_radius_m;
    double j2;
    std::string_view model;
    double j4 = 0;
};
// Fixed nominal constants from IERS Conventions 2003, Table 1.1.
// The dynamical reference radius is independent of the display's WGS84 ellipsoid.
[[nodiscard]] inline const CentralBody &central_body(OriginId origin) {
    static constexpr CentralBody earth{
        OriginId::earth, {3.986004418e14}, 6378136.6, 1.0826359e-3, "earth-iers2003-v1"};
    if (origin != OriginId::earth)
        throw std::invalid_argument("Unsupported central body");
    return earth;
}
} // namespace orbitforge::core
