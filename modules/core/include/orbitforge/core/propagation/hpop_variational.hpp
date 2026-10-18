#pragma once
#include <orbitforge/core/propagation/hpop.hpp>

namespace orbitforge::core {
struct HpopGravityTruncation {
    int degree = 4, order = 4;
    bool operator==(const HpopGravityTruncation &) const = default;
};
struct HpopVariationalOptions {
    // Empty uses the nominal gravity truncation. Independent field limits and
    // existing tide/secular rules apply; nominal acceleration is never truncated.
    std::optional<HpopGravityTruncation> gravity;
    // Absolute local tolerance for dimensionless variational states. Scaling is
    // 1e6 m, 1e3 m/s, and 1 m^2/kg for D/K; valid range is 1e-16..1.
    // Relative tolerance comes from HpopOptions.
    double absolute_tolerance = 1e-14;
};
struct HpopVariationalState {
    CartesianState state;
    // d(r,v)(t)/d(r,v)(initial), at fixed physical TAI endpoints, row-major.
    // Blocks have units 1, s, 1/s, 1. Initial value is identity. Independent gravity
    // truncation gives the selected covariance approximation to this derivative.
    std::array<std::array<double, 6>, 6> state_transition{};
    // d(r,v)(t)/dD and d(r,v)(t)/dK, D=Cd*DragArea/Mass,
    // K=Cr*SRPArea/Mass [m^2/kg]. Units kg/m and kg/(m*s).
    // Initial values are zero, including when nominal Cd or Cr is zero.
    std::array<double, 6> drag_sensitivity{}, srp_sensitivity{};
};
// Integrate one coast, state + STM + two constant-parameter sensitivities with
// the selected HPOP method. Earth/GCRF, TAI Epoch, SI. Same resource, duration,
// altitude, cancellation and work limits as propagate_hpop_ephemeris. No stages
// outside [initial.epoch,end], no interpolation or maneuver crossing. Physical
// fixed-time variational equations also apply with VOP and Sundman integration.
// Spherical dual-cone shadow and external weather time gates retain continuous
// variational states. Cylindrical shadow and zero-width atmosphere switching are
// currently rejected: their state-dependent force jumps require saltation.
// Existing local derivative pole/branch restrictions apply. This is not a
// covariance matrix, process-noise model or a derivative of integrator error.
[[nodiscard]] HpopVariationalState propagate_hpop_variational(
    CartesianState initial, Epoch end, HpopOptions options = {}, HpopEnvironment environment = {},
    HpopVariationalOptions variational = {}, const std::function<void()> &checkpoint = {});
} // namespace orbitforge::core
