#pragma once
#include <array>

namespace orbitforge::core {
// Received Sun vector relative to the spacecraft, in metres in ICRF-aligned
// axes. Fixed receiving Epoch and ephemeris. Input position is Earth relative
// in metres; input velocity is Earth relative in metres per TAI second.
// Rows are output vector components, columns are observer-state components.
struct ApparentSunDifferential {
    std::array<double, 3> position_m{};
    std::array<std::array<double, 3>, 3> position_jacobian{}; // dimensionless
    std::array<std::array<double, 3>, 3> velocity_jacobian_s{};
};
} // namespace orbitforge::core
