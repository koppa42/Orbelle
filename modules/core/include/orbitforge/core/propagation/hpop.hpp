#pragma once
#include <memory>
#include <optional>
#include <orbitforge/core/frames/frame_provider.hpp>
#include <orbitforge/core/propagation/gravity.hpp>
#include <orbitforge/core/propagation/solar_radiation_pressure.hpp>
#include <orbitforge/core/propagation/state_provider.hpp>
#include <orbitforge/core/propagation/third_body.hpp>
#include <string_view>
#include <vector>

namespace orbitforge::core {
// Static, tide-free EGM96, truncated to degree/order 20. No tides or secular drift.
inline constexpr double egm96_mu = 3.986004415e14;
inline constexpr double egm96_radius_m = 6378136.3;
inline constexpr int egm96_max_degree = 20;
inline constexpr double hpop_max_duration_seconds = 366.0 * 86400;
enum class HpopIntegrator { rkf78, rk4 };
struct HpopOptions {
    // Catalog selection, never a file path. Empty selects the built-in 20x20 EGM96.
    std::string gravity_model{};
    // Selected resource's GM, also used when converting initial osculating elements.
    // Resource preparation verifies this value against the file; it cannot override it.
    double gravity_mu = egm96_mu;
    std::string ephemeris_model = "de440s";
    std::array<ThirdBodyGravity, third_body_count> third_bodies{};
    SrpOptions srp;
    [[nodiscard]] ThirdBodyGravity &third_body(ThirdBody body) {
        return third_bodies.at(static_cast<std::size_t>(body));
    }
    [[nodiscard]] const ThirdBodyGravity &third_body(ThirdBody body) const {
        return third_bodies.at(static_cast<std::size_t>(body));
    }
    [[nodiscard]] std::vector<ThirdBody> enabled_third_bodies() const {
        std::vector<ThirdBody> result;
        for (auto body : third_body_list)
            if (third_body(body).enabled)
                result.push_back(body);
        return result;
    }
    [[nodiscard]] bool uses_third_bodies() const {
        for (const auto &body : third_bodies)
            if (body.enabled)
                return true;
        return false;
    }
    [[nodiscard]] bool uses_ephemeris() const { return uses_third_bodies() || srp.enabled; }
    [[nodiscard]] std::vector<ThirdBody> required_ephemeris_bodies() const {
        std::vector<ThirdBody> result;
        for (auto body : third_body_list)
            if (third_body(body).enabled ||
                (srp.enabled &&
                 (body == ThirdBody::sun || (body == ThirdBody::moon && srp.moon_shadow &&
                                             srp.shadow_model != SrpShadowModel::none))))
                result.push_back(body);
        return result;
    }
    // 0/0 selects the chosen model's monopole. File models support up to 360x360.
    int gravity_degree = 8, gravity_order = 8;
    HpopIntegrator integrator = HpopIntegrator::rkf78;
    double relative_tolerance = 1e-12;
    double position_tolerance_m = 1e-3, velocity_tolerance_mps = 1e-6;
    double min_step_seconds = 0.001, max_step_seconds = 60;
    double fixed_step_seconds = 10; // RK4 only; no error control.
    // Geocentric altitude above the selected model's reference sphere, at each force evaluation.
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
    std::shared_ptr<const GravityModel> gravity{};
    std::shared_ptr<const IThirdBodyEphemeris> ephemeris{};
};
// Cowell equations in GCRF, TAI seconds. Initial state is osculating, not mean elements.
// Deterministic checkpoints are independent of query order and output sampling.
// Supports forward/backward propagation within 366 days of the initial epoch.
// No implicit data loading or extrapolation. PropagationError on impact, a failed
// tolerance or work limit. Cancellation checkpoints may throw; no partial result.
[[nodiscard]] std::shared_ptr<const IStateProvider>
make_hpop(CartesianState initial, HpopOptions options = {}, HpopEnvironment environment = {});
} // namespace orbitforge::core
