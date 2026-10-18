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
    // FK5 J2000 / TrueOfDate through the pseudo-fixed frame. Model and EOP
    // identity are returned explicitly; these are not aliases for GCRF/SPICE.
    [[nodiscard]] virtual PositionRotation gcrf_to_celestial(Epoch, const EarthOrientationSample &,
                                                             FrameId) const {
        throw std::logic_error("This frame provider does not support FK5 celestial frames");
    }
    [[nodiscard]] virtual StateTransform
    gcrf_to_celestial_state(Epoch, const EarthOrientationState &, FrameId) const {
        throw std::logic_error("This frame provider does not support FK5 celestial frames");
    }
    // TEME of date: GMST82 to the pseudo Earth-fixed frame, then polar motion.
    [[nodiscard]] virtual PositionRotation teme_to_itrf(Epoch,
                                                        const EarthOrientationSample &) const {
        throw std::logic_error("This frame provider does not support TEME");
    }
    [[nodiscard]] virtual StateTransform teme_to_itrf_state(Epoch,
                                                            const EarthOrientationState &) const {
        throw std::logic_error("This frame provider does not support TEME");
    }
};
// GCRF/ITRF/TEME route through ITRF; FK5 endpoints route through GCRF.
// Includes rotation-rate velocity terms. Never aliases different reference frames.
[[nodiscard]] CartesianState transform_state(const CartesianState &state, FrameId target,
                                             const IFrameProvider &frames,
                                             const EarthOrientationState &eop);
} // namespace orbitforge::core
