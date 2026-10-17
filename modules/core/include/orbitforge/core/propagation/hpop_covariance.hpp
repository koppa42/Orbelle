#pragma once
#include <orbitforge/core/propagation/hpop_variational.hpp>

namespace orbitforge::core {
using HpopStateCovariance = std::array<std::array<double, 6>, 6>;
struct HpopConsiderParameter {
    // Self-covariance of D or K, (m^2/kg)^2, NOT sigma or a correlation coefficient.
    double variance = 0;
    // Cov(parameter, [r,v]) in Earth/GCRF: m^3/kg, m^3/(kg*s).
    std::array<double, 6> state_cross_covariance{};
    bool operator==(const HpopConsiderParameter &) const = default;
};
struct HpopCovarianceInput {
    // Symmetric Cartesian covariance, row/column order rx,ry,rz,vx,vy,vz.
    // SI blocks m^2, m^2/s, m^2/s^2; Earth/GCRF at the initial state epoch.
    // Zero initialization is deliberately invalid: no uncertainty is invented.
    HpopStateCovariance state{};
    // Absence excludes the parameter. Active joint covariance must be positive
    // definite, so an active zero variance is rejected; omit a known parameter.
    std::optional<HpopConsiderParameter> drag, srp;
    // Cov(D,K), (m^2/kg)^2. Nonzero requires both parameters to be present.
    double drag_srp_covariance = 0;
    bool operator==(const HpopCovarianceInput &) const = default;
};
struct HpopCovarianceContributions {
    HpopStateCovariance initial_state{}, consider_parameters{}, state_parameter_cross{};
};
struct HpopCovarianceState {
    HpopVariationalState variational;
    // Same structure at variational.state.epoch. Parameter variances and Cov(D,K)
    // stay constant; parameter/state cross covariances evolve and can be reused
    // as the next coast's input. No process noise or maneuver noise is added.
    HpopCovarianceInput covariance;
    // Phi Pzz Phi^T, S Pww S^T, Phi Pzw S^T + transpose. The cross contribution
    // may be indefinite. Their floating-point sum can differ by rounding from
    // the total, which is evaluated directly through the joint square root.
    HpopCovarianceContributions contributions;
};
// Finite, symmetric and positive definite ACTIVE joint matrix (6, 7 or 8 dims).
// Symmetry allows 32*epsilon in correlation-normalized entries and averages
// that roundoff. Diagonal scaling precedes Cholesky. No jitter, clipping, nearest
// PSD repair or implicit removal of a requested parameter; throws invalid_argument.
void validate_hpop_covariance(const HpopCovarianceInput &input);
// Apply the supplied physical fixed-time STM and D/K sensitivities to input.
// Caller must associate them with this initial covariance's epoch/state/model.
// Earth/GCRF only. Nonfinite or numerically non-PD output throws PropagationError.
// Both input and variational data are copied into the result; cancellation throws
// without publishing partial output. The input is never modified.
[[nodiscard]] HpopCovarianceState
transport_hpop_covariance(HpopCovarianceInput input, HpopVariationalState variational,
                          const std::function<void()> &checkpoint = {});
// One bounded coast with the same methods, resources, event restrictions and
// cancellation contract as propagate_hpop_variational. No interpolation or frame
// conversion; returns the complete active joint covariance at the physical endpoint.
[[nodiscard]] HpopCovarianceState
propagate_hpop_covariance(CartesianState initial, Epoch end, HpopCovarianceInput covariance,
                          HpopOptions options = {}, HpopEnvironment environment = {},
                          HpopVariationalOptions variational = {},
                          const std::function<void()> &checkpoint = {});
} // namespace orbitforge::core
