#include <cmath>
#include <orbelle/application/hpop_covariance.hpp>

namespace orbelle::application {
core::HpopCovarianceInput HpopCovarianceSettings::active_input() const {
    core::HpopCovarianceInput result;
    result.state = state;
    result.coordinates = coordinates;
    if (consider_analysis) {
        if (drag_enabled)
            result.drag = drag;
        if (srp_enabled)
            result.srp = srp;
        if (consider_cross_correlation && drag_enabled && srp_enabled)
            result.drag_srp_covariance = drag_srp_covariance;
    }
    return result;
}
core::HpopVariationalOptions HpopCovarianceSettings::variational_options() const {
    return {independent_gravity ? std::optional{gravity} : std::nullopt, absolute_tolerance};
}
bool HpopCovarianceSettings::uses_celestial_frame() const {
    return enabled && (coordinates.frame == core::HpopCovarianceFrame::fk5_j2000 ||
                       coordinates.frame == core::HpopCovarianceFrame::true_of_date);
}
void validate_hpop_covariance_settings(const HpopCovarianceSettings &s,
                                       const core::HpopOptions &force) {
    const auto require = [](bool valid, const char *message) {
        if (!valid)
            throw std::invalid_argument(message);
    };
    require(s.coordinates.frame >= core::HpopCovarianceFrame::gcrf &&
                s.coordinates.frame <= core::HpopCovarianceFrame::true_of_date &&
                s.coordinates.velocity >= core::HpopCovarianceVelocity::projected &&
                s.coordinates.velocity <= core::HpopCovarianceVelocity::rotating,
            "Invalid covariance coordinates");
    require((s.gravity.degree == 0 || s.gravity.degree >= 2) &&
                s.gravity.degree <= core::gravity_max_degree && s.gravity.order >= 0 &&
                s.gravity.order <= s.gravity.degree,
            "Covariance gravity requires 0/0 or degree 2..2190 and order 0..degree");
    require(std::isfinite(s.absolute_tolerance) && s.absolute_tolerance >= 1e-16 &&
                s.absolute_tolerance <= 1,
            "Covariance absolute tolerance must be 1e-16..1");
    for (std::size_t i = 0; i < 6; ++i)
        for (std::size_t j = 0; j < 6; ++j)
            require(std::isfinite(s.state[i][j]) && s.state[i][j] == s.state[j][i] &&
                        (i != j || s.state[i][j] >= 0),
                    "Covariance draft must be finite, symmetric, with nonnegative diagonal");
    for (const auto *parameter : {&s.drag, &s.srp}) {
        require(std::isfinite(parameter->variance) && parameter->variance >= 0,
                "Consider variance must be finite and nonnegative");
        for (double value : parameter->state_cross_covariance)
            require(std::isfinite(value), "Consider cross covariance must be finite");
    }
    require(std::isfinite(s.drag_srp_covariance), "D/K cross covariance must be finite");
    if (!s.enabled)
        return;
    core::validate_hpop_covariance(s.active_input());
    if (force.srp.enabled && force.srp.shadow_model == core::SrpShadowModel::cylindrical &&
        (force.srp.earth_shadow || force.srp.moon_shadow ||
         !force.srp.additional_eclipsing_bodies.empty()))
        throw std::invalid_argument(
            "Covariance with cylindrical shadows requires event jump propagation");
    if (force.drag.enabled && force.drag.atmosphere == core::AtmosphereModel::jacchia_roberts &&
        force.drag.low_altitude_model != core::LowAltitudeDensityModel::none &&
        force.drag.blending_range_m == 0)
        throw std::invalid_argument(
            "Covariance with hard atmosphere switching requires event jump propagation");
}
} // namespace orbelle::application
