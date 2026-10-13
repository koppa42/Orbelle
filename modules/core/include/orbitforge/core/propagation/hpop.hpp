#pragma once
#include <memory>
#include <orbitforge/core/frames/frame_provider.hpp>
#include <orbitforge/core/propagation/state_provider.hpp>
#include <string_view>

namespace orbitforge::core {
// Static, tide-free EGM96, truncated to degree/order 20. No tides or secular drift.
inline constexpr double egm96_mu = 3.986004415e14;
inline constexpr double egm96_radius_m = 6378136.3;
inline constexpr int egm96_max_degree = 20;
inline constexpr double hpop_max_duration_seconds = 366.0 * 86400;
enum class HpopIntegrator { rkf78, rk4 };
struct HpopOptions {
    // 0/0 selects point mass; 2..20 includes all terms through degree/order.
    int gravity_degree = 8, gravity_order = 8;
    HpopIntegrator integrator = HpopIntegrator::rkf78;
    double relative_tolerance = 1e-12;
    double position_tolerance_m = 1e-3, velocity_tolerance_mps = 1e-6;
    double min_step_seconds = 0.001, max_step_seconds = 60;
    double fixed_step_seconds = 10; // RK4 only; no error control.
    // Geocentric altitude above the EGM96 reference sphere, at every force evaluation.
    double minimum_altitude_m = 0;
    bool operator==(const HpopOptions &) const = default;
};
void validate_hpop_options(const HpopOptions &options);
// Includes the point-mass term. SI, in Earth-fixed axes; nonsingular at the poles.
[[nodiscard]] std::array<double, 3> egm96_acceleration(const std::array<double, 3> &position_m,
                                                       int degree, int order);
[[nodiscard]] std::string_view egm96_model_id() noexcept;
struct HpopEnvironment {
    // Required for noncentral gravity. Shared immutable ownership, concurrent reads.
    std::shared_ptr<const IFrameProvider> frames;
    std::shared_ptr<const IEarthOrientationProvider> eop;
};
// Cowell equations in GCRF, TAI seconds. Initial state is osculating, not mean elements.
// Deterministic checkpoints are independent of query order and output sampling.
// Supports forward/backward propagation within 366 days of the initial epoch.
// No implicit data loading or extrapolation. PropagationError on impact, a failed
// tolerance or work limit. Cancellation checkpoints may throw; no partial result.
[[nodiscard]] std::shared_ptr<const IStateProvider>
make_hpop(CartesianState initial, HpopOptions options = {}, HpopEnvironment environment = {});
} // namespace orbitforge::core
