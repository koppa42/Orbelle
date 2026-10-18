#pragma once
#include <orbitforge/core/propagation/hpop_covariance.hpp>

namespace orbitforge::application {
// Editable SI covariance at OrbitInput.initial.epoch. Disabled fields remain
// finite drafts; only enabled fields participate in propagation and identity.
// No initial uncertainty is guessed: enabling a zero matrix is an error.
struct HpopCovarianceSettings {
    bool enabled = false;
    core::HpopStateCovariance state{};
    core::HpopCovarianceCoordinates coordinates{};
    bool independent_gravity = false;
    core::HpopGravityTruncation gravity{};
    double absolute_tolerance = 1e-14;
    bool consider_analysis = false;
    bool drag_enabled = false, srp_enabled = false;
    core::HpopConsiderParameter drag, srp;
    bool consider_cross_correlation = false;
    double drag_srp_covariance = 0;
    [[nodiscard]] core::HpopCovarianceInput active_input() const;
    [[nodiscard]] core::HpopVariationalOptions variational_options() const;
    [[nodiscard]] bool uses_celestial_frame() const;
    bool operator==(const HpopCovarianceSettings &) const = default;
};
// Checks all stored fields, including finite disabled drafts. When enabled,
// checks the active joint matrix and supported force discontinuities.
// Resource availability is checked by Runtime against the selected snapshot.
void validate_hpop_covariance_settings(const HpopCovarianceSettings &settings,
                                       const core::HpopOptions &force);
} // namespace orbitforge::application
