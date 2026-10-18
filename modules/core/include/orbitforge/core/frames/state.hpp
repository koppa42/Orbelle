#pragma once
#include <array>
#include <orbitforge/core/time/epoch.hpp>

namespace orbitforge::core {
enum class FrameId { gcrf, itrf, spice_j2000, teme, fk5_j2000, true_of_date };
enum class OriginId { earth };

struct CartesianState {
    Epoch epoch;
    OriginId origin;
    FrameId frame;
    std::array<double, 3> position_m;
    std::array<double, 3> velocity_mps;
};
} // namespace orbitforge::core
