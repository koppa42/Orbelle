#pragma once
#include <array>
#include <orbitforge/core/frames/eop_provider.hpp>
#include <orbitforge/core/frames/state.hpp>
#include <string>

namespace orbitforge::core {
using Matrix3 = std::array<std::array<double, 3>, 3>;

struct PositionRotation {
    Epoch epoch;
    OriginId origin;
    FrameId from;
    FrameId to;
    Matrix3 matrix; // Row-major; r_to = matrix * r_from.
    std::string model;
    std::string eop_source_id;
    // Positions only. Velocity also needs the time derivative of the rotation.
    [[nodiscard]] std::array<double, 3>
    apply_position(const std::array<double, 3> &position_m) const;
    [[nodiscard]] std::array<double, 3>
    inverse_position(const std::array<double, 3> &position_m) const;
};

struct StateTransform {
    PositionRotation rotation;
    Matrix3 rotation_rate_per_second;
    std::string derivative_model;
    [[nodiscard]] CartesianState apply(const CartesianState &state) const;
    [[nodiscard]] CartesianState inverse(const CartesianState &state) const;
};

class IFrameProvider {
  public:
    virtual ~IFrameProvider() = default;
    [[nodiscard]] virtual PositionRotation
    gcrf_to_itrf(Epoch epoch, const EarthOrientationSample &eop) const = 0;
    [[nodiscard]] virtual StateTransform
    gcrf_to_itrf_state(Epoch epoch, const EarthOrientationState &eop) const = 0;
};
} // namespace orbitforge::core
