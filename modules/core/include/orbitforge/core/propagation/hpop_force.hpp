#pragma once
#include <orbitforge/core/propagation/hpop.hpp>

namespace orbitforge::core {
struct HpopForceDifferential {
    ForceDifferential force{};
    // STK consider parameters D=Cd*DragArea/Mass and K=Cr*SRPArea/Mass [m^2/kg].
    // Output derivatives are inertial vectors in Pa, including at D=0 or K=0.
    std::array<double, 3> drag_parameter_jacobian_pa{};
    std::array<double, 3> srp_parameter_jacobian_pa{};
};
// Instantaneous total HPOP acceleration; same evaluation order and models as
// propagation. Earth-centred GCRF, TAI Epoch and SI state. Explicit immutable
// resources; empty gravity selection uses the built-in field as in make_hpop.
// Validates options, resources, point coverage, state and minimum altitude.
// No integration, implicit downloads, interpolation or event-side override.
[[nodiscard]] std::array<double, 3> evaluate_hpop_acceleration(const CartesianState &state,
                                                               const HpopOptions &options,
                                                               HpopEnvironment environment = {});
// Full local force derivatives at fixed force epoch, external resources and
// parameters. Includes gravity corrections, relativistic terms, each selected
// third body, SRP Sun-state chain and the full atmosphere rotation-rate coupling.
// Rows/columns and units follow ForceDifferential. The nominal and differential
// gravity truncations currently coincide. Existing force derivative branch and
// pole restrictions apply; unsupported derivatives fail, never disappear from
// the sum. Not an STM, covariance, event jump or integrator-time derivative.
// Checkpoint may throw; no partial result or shared input mutation is published.
[[nodiscard]] HpopForceDifferential
hpop_force_differential(const CartesianState &state, const HpopOptions &options,
                        HpopEnvironment environment = {},
                        const std::function<void()> &checkpoint = {});
} // namespace orbitforge::core
