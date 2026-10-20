#pragma once
#include <orbitforge/core/propagation/hpop_variational.hpp>
#include <vector>

namespace orbitforge::core {
using HpopStateCovariance = std::array<std::array<double, 6>, 6>;
// STK LVLH = (radial, in-track, orbit normal), NOT CCSDS LVLH/VVLH.
// STK Frenet = (N,T,C): T along inertial velocity, C along r cross v.
enum class HpopCovarianceFrame { gcrf, lvlh, frenet, fk5_j2000, true_of_date };
struct HpopCovarianceFrameEnvironment {
    std::shared_ptr<const IFrameProvider> frames;
    std::shared_ptr<const IEarthOrientationProvider> eop;
};
// projected: components of inertial position/velocity errors along the axes.
// rotating: velocity is the time derivative relative to the rotating axes.
enum class HpopCovarianceVelocity { projected, rotating };
struct HpopCovarianceCoordinates {
    HpopCovarianceFrame frame = HpopCovarianceFrame::gcrf;
    HpopCovarianceVelocity velocity = HpopCovarianceVelocity::projected;
    bool operator==(const HpopCovarianceCoordinates &) const = default;
};
// Cartesian error-state Jacobian at one fixed nominal reference state. Blocks
// have units 1, s, 1/s, 1. Axes are held at the nominal trajectory, not recomputed
// from each perturbed sample. Earth/GCRF reference, TAI seconds and SI.
// A rotating local frame requires the actual nominal inertial acceleration;
// no Kepler acceleration is guessed. Projected frames need only r and v.
// Nonfinite/zero r or v and unresolved |rhat cross vhat| <= 64*epsilon fail.
// FK5 J2000 and TrueOfDate require explicit frames/EOP, even for identity;
// the selected provider defines the nutation model and data coverage. No local
// acceleration is needed for celestial axes. Environment is captured by value.
// GCRF projected/rotating coincide. Invalid input throws invalid_argument;
// nonrepresentable finite-range results throw PropagationError.
[[nodiscard]] HpopStateCovariance
hpop_covariance_frame_jacobian(const CartesianState &reference, HpopCovarianceCoordinates from,
                               HpopCovarianceCoordinates to,
                               std::optional<std::array<double, 3>> acceleration_mps2 = {},
                               HpopCovarianceFrameEnvironment environment = {});
struct HpopConsiderParameter {
    // Self-covariance of D or K, (m^2/kg)^2, NOT sigma or a correlation coefficient.
    double variance = 0;
    // Cov(parameter, [r,v]) in input.coordinates: m^3/kg, m^3/(kg*s).
    std::array<double, 6> state_cross_covariance{};
    bool operator==(const HpopConsiderParameter &) const = default;
};
struct HpopCovarianceInput {
    // Symmetric Cartesian covariance, row/column order rx,ry,rz,vx,vy,vz.
    // SI blocks m^2, m^2/s, m^2/s^2; Earth-centred, at the initial state epoch.
    // Zero initialization is deliberately invalid: no uncertainty is invented.
    HpopStateCovariance state{};
    // Absence excludes the parameter. Active joint covariance must be positive
    // definite, so an active zero variance is rejected; omit a known parameter.
    std::optional<HpopConsiderParameter> drag, srp;
    // Cov(D,K), (m^2/kg)^2. Nonzero requires both parameters to be present.
    double drag_srp_covariance = 0;
    HpopCovarianceCoordinates coordinates{};
    bool operator==(const HpopCovarianceInput &) const = default;
};
struct HpopCovarianceContributions {
    HpopStateCovariance initial_state{}, consider_parameters{}, state_parameter_cross{};
};
struct HpopCovarianceState {
    HpopVariationalState variational;
    // Earth/GCRF projected, at variational.state.epoch. Parameter variances and Cov(D,K)
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
// Re-express the complete active joint covariance at reference.epoch. D/K
// variances and their mutual covariance stay exact; their state cross terms
// transform with the same Jacobian. Caller associates the matrix with this
// nominal reference state/epoch. Does not propagate time or change the orbit.
// Input is captured before callbacks; no partial result on cancellation/failure.
[[nodiscard]] HpopCovarianceInput convert_hpop_covariance(
    CartesianState reference, HpopCovarianceInput input, HpopCovarianceCoordinates target,
    std::optional<std::array<double, 3>> acceleration_mps2 = {},
    const std::function<void()> &checkpoint = {}, HpopCovarianceFrameEnvironment environment = {});
// Apply the supplied physical fixed-time STM and D/K sensitivities to input.
// Caller must associate them with this initial covariance's epoch/state/model.
// Earth/GCRF only. Nonfinite or numerically non-PD output throws PropagationError.
// Both input and variational data are copied into the result; cancellation throws
// without publishing partial output. The input is never modified.
[[nodiscard]] HpopCovarianceState
transport_hpop_covariance(HpopCovarianceInput input, HpopVariationalState variational,
                          const std::function<void()> &checkpoint = {});
// One bounded coast with the same methods, resources, event restrictions and
// cancellation contract as propagate_hpop_variational. No covariance
// interpolation. Local-frame input is explicitly converted at the initial state;
// rotating axes use the actual instantaneous nominal HPOP acceleration at that
// epoch (no event-side override). Output and all three contributions are GCRF.
// Returns the complete active joint covariance at the physical endpoint.
[[nodiscard]] HpopCovarianceState
propagate_hpop_covariance(CartesianState initial, Epoch end, HpopCovarianceInput covariance,
                          HpopOptions options = {}, HpopEnvironment environment = {},
                          HpopVariationalOptions variational = {},
                          const std::function<void()> &checkpoint = {});
// Sample up to 200000 epochs without restarting at the initial state for each
// output. Times/inputs are captured by value before callbacks. Each side of the
// initial epoch advances once, retaining the global STM and integration history.
// Output epochs are exact integration stops, not interpolated covariances; their
// spacing can affect the numerical path within the selected method's accuracy.
// Same force/event/resource restrictions as propagate_hpop_covariance; 200000
// integration attempts per direction and at most 366 days from initial. Unsorted
// times and duplicates are supported. The callback receives the original index
// in propagation order (not necessarily chronological), and a temporary result.
// Full coverage is checked before the first sample. Cancellation or a later
// numerical failure throws; consumers must discard any earlier streamed samples.
void sample_hpop_covariance(
    CartesianState initial, std::vector<Epoch> times, HpopCovarianceInput covariance,
    const std::function<void(std::size_t, const HpopCovarianceState &)> &sample,
    HpopOptions options = {}, HpopEnvironment environment = {},
    HpopVariationalOptions variational = {}, const std::function<void()> &checkpoint = {});
} // namespace orbitforge::core
