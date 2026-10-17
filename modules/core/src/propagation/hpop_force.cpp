#include "hpop_force.hpp"
#include "srp_geometry.hpp"
#include <algorithm>
#include <cmath>
#include <type_traits>

namespace orbitforge::core {
namespace {
using Vector = std::array<double, 3>;
using Matrix = std::array<Vector, 3>;
std::function<double(Epoch)> sun_distance(const IThirdBodyEphemeris *ephemeris) {
    return [ephemeris](Epoch at) {
        const auto p = ephemeris->position(ThirdBody::sun, at);
        return std::hypot(p[0], p[1], p[2]);
    };
}
void add_blocks(ForceDifferential &total, const ForceDifferential &part) {
    for (unsigned i = 0; i < 3; ++i)
        for (unsigned j = 0; j < 3; ++j) {
            total.position_jacobian_per_s2[i][j] += part.position_jacobian_per_s2[i][j];
            total.velocity_jacobian_per_s[i][j] += part.velocity_jacobian_per_s[i][j];
        }
}
ForceDifferential inertial_blocks(const ForceDifferential &fixed, const PositionRotation &rotation,
                                  const Matrix &rate = {}) {
    ForceDifferential result;
    const auto &r = rotation.matrix;
    // a = R^T a_fixed(R position, R velocity + Rdot position).
    for (unsigned i = 0; i < 3; ++i)
        for (unsigned j = 0; j < 3; ++j)
            for (unsigned k = 0; k < 3; ++k)
                for (unsigned l = 0; l < 3; ++l) {
                    result.position_jacobian_per_s2[i][j] +=
                        r[k][i] * (fixed.position_jacobian_per_s2[k][l] * r[l][j] +
                                   fixed.velocity_jacobian_per_s[k][l] * rate[l][j]);
                    result.velocity_jacobian_per_s[i][j] +=
                        r[k][i] * fixed.velocity_jacobian_per_s[k][l] * r[l][j];
                }
    return result;
}
void finite(const Vector &values) {
    for (double value : values)
        if (!std::isfinite(value))
            throw PropagationError("HPOP force or derivative is nonfinite");
}
template <bool Differential>
auto evaluate_force(const CartesianState &state, const HpopOptions &options,
                    const HpopEnvironment &environment, std::optional<Epoch> weather_at,
                    std::optional<double> shadow_lighting,
                    const std::function<void()> &checkpoint = {}) {
    using Result = std::conditional_t<Differential, HpopForceDifferential, Vector>;
    Result result{};
    Vector &acceleration = [&]() -> Vector & {
        if constexpr (Differential)
            return result.force.acceleration_mps2;
        else
            return result;
    }();
    const auto check = [&] {
        if constexpr (Differential)
            if (checkpoint)
                checkpoint();
    };
    const auto &r = state.position_m;
    const auto &v = state.velocity_mps;
    const auto epoch = state.epoch;
    check();
    if (!options.uses_earth_fixed_gravity()) {
        if constexpr (Differential) {
            const auto part = environment.gravity->differential(r, 0, 0, {}, {}, checkpoint);
            acceleration = part.acceleration_mps2;
            result.force.position_jacobian_per_s2 = part.position_jacobian_per_s2;
        } else {
            acceleration = environment.gravity->acceleration(r, 0, 0);
        }
    } else {
        const auto eop = environment.eop->at(epoch).sample;
        const auto rotation = environment.frames->gcrf_to_itrf(epoch, eop);
        std::array<TideRaisingBody, 2> bodies{};
        if (options.uses_full_solid_tides()) {
            const auto &info = environment.ephemeris->data_info();
            bodies = {
                {{rotation.apply_position(environment.ephemeris->position(ThirdBody::sun, epoch)),
                  info.sun_mu},
                 {rotation.apply_position(environment.ephemeris->position(ThirdBody::moon, epoch)),
                  info.moon_mu}}};
        }
        auto corrections = solid_tide_corrections(environment.gravity->info(), options.solid_tides,
                                                  options.gravity_degree, options.gravity_order,
                                                  bodies, &eop, environment.time.get());
        if (options.ocean_tides.active()) {
            auto ocean = environment.ocean_tides->corrections(environment.gravity->info(), eop,
                                                              options.ocean_tides.degree,
                                                              options.ocean_tides.order);
            // Keep the existing nominal merge and summation order.
            for (const auto &solid : corrections) {
                const auto found = std::ranges::find_if(ocean, [&](const auto &term) {
                    return term.degree == solid.degree && term.order == solid.order;
                });
                if (found == ocean.end())
                    ocean.push_back(solid);
                else {
                    found->c += solid.c;
                    found->s += solid.s;
                }
            }
            corrections = std::move(ocean);
        }
        const auto secular = options.uses_secular_gravity() ? std::optional{epoch} : std::nullopt;
        if constexpr (Differential) {
            const auto part = environment.gravity->differential(
                rotation.apply_position(r), options.gravity_degree, options.gravity_order,
                corrections, secular, checkpoint);
            acceleration = rotation.inverse_position(part.acceleration_mps2);
            add_blocks(result.force,
                       inertial_blocks({{}, part.position_jacobian_per_s2, {}}, rotation));
        } else {
            acceleration = rotation.inverse_position(environment.gravity->acceleration(
                rotation.apply_position(r), options.gravity_degree, options.gravity_order,
                corrections, secular));
        }
    }
    check();
    if (options.include_relativistic_accelerations) {
        const auto rotation =
            environment.frames->gcrf_to_itrf(epoch, environment.eop->at(epoch).sample);
        const auto sun = environment.ephemeris->geometric_state(ThirdBody::sun, epoch);
        Vector part;
        if constexpr (Differential) {
            const auto force =
                relativity_differential(r, v, options.gravity_mu, sun.position_m, sun.velocity_mps,
                                        environment.ephemeris->data_info().sun_mu,
                                        rotation.inverse_position({0, 0, 1}))
                    .total;
            part = force.acceleration_mps2;
            add_blocks(result.force, force);
        } else {
            part = evaluate_relativity(r, v, options.gravity_mu, sun.position_m, sun.velocity_mps,
                                       environment.ephemeris->data_info().sun_mu,
                                       rotation.inverse_position({0, 0, 1}))
                       .acceleration_mps2;
        }
        for (unsigned i = 0; i < 3; ++i)
            acceleration[i] += part[i];
    }
    if (options.uses_third_bodies()) {
        const auto &info = environment.ephemeris->data_info();
        for (auto body : third_body_list) {
            if (!options.third_body(body).enabled)
                continue;
            check();
            const double mu = third_body_mu(options.third_body(body), body, info);
            const auto p = environment.ephemeris->position(body, epoch);
            Vector part;
            if constexpr (Differential) {
                const auto force = third_body_differential(r, p, mu);
                part = force.acceleration_mps2;
                add_blocks(result.force, force);
            } else {
                part = third_body_acceleration(r, p, mu);
            }
            for (unsigned i = 0; i < 3; ++i)
                acceleration[i] += part[i];
        }
    }
    check();
    if (options.srp.enabled) {
        Vector part;
        if constexpr (Differential) {
            const auto force = srp_state_differential(state, options.srp, *environment.ephemeris);
            part = force.force.acceleration_mps2;
            add_blocks(result.force, force.force);
            result.srp_parameter_jacobian_pa = force.srp_parameter_jacobian_pa;
        } else {
            const auto geometry =
                detail::resolve_srp_geometry(epoch, r, v, options.srp, *environment.ephemeris);
            auto force = evaluate_srp(r, geometry.sun, options.srp, geometry.moon,
                                      std::span{geometry.additional}.first(geometry.count));
            if (shadow_lighting) {
                auto unshadowed = options.srp;
                unshadowed.shadow_model = SrpShadowModel::none;
                force = evaluate_srp(r, geometry.sun, unshadowed);
                for (auto &component : force.acceleration_mps2)
                    component *= *shadow_lighting;
            }
            part = force.acceleration_mps2;
        }
        for (unsigned i = 0; i < 3; ++i)
            acceleration[i] += part[i];
    }
    check();
    if (options.drag.enabled) {
        const auto transform =
            environment.frames->gcrf_to_itrf_state(epoch, environment.eop->at(epoch));
        const auto fixed = transform.apply(state);
        const auto sun = options.drag.use_apparent_sun_position
                             ? environment.ephemeris->apparent_sun_from_earth(epoch)
                             : environment.ephemeris->position(ThirdBody::sun, epoch);
        std::optional<SpaceWeatherSample> weather, lower;
        if (!options.drag.weather_model.empty()) {
            const auto &d = options.drag;
            weather = environment.weather->sample(
                weather_at.value_or(epoch), *environment.time, d.atmosphere, d.geomagnetic_update,
                d.geomagnetic_source, sun_distance(environment.ephemeris.get()));
            if (d.atmosphere == AtmosphereModel::jacchia_roberts &&
                d.low_altitude_model == LowAltitudeDensityModel::nrlmsise00)
                lower = environment.weather->sample(weather_at.value_or(epoch), *environment.time,
                                                    AtmosphereModel::nrlmsise00,
                                                    d.geomagnetic_update, d.geomagnetic_source,
                                                    sun_distance(environment.ephemeris.get()));
        }
        Vector part;
        if constexpr (Differential) {
            const auto force = drag_differential(
                fixed.position_m, fixed.velocity_mps, transform.rotation.apply_position(sun),
                environment.time->to_utc(epoch), options.drag, weather, lower);
            part = force.nominal.acceleration_itrf_mps2;
            add_blocks(result.force, inertial_blocks(force.force, transform.rotation,
                                                     transform.rotation_rate_per_second));
            result.drag_parameter_jacobian_pa =
                transform.rotation.inverse_position(force.drag_parameter_jacobian_pa);
        } else {
            part = evaluate_drag(fixed.position_m, fixed.velocity_mps,
                                 transform.rotation.apply_position(sun),
                                 environment.time->to_utc(epoch), options.drag, weather, lower)
                       .acceleration_itrf_mps2;
        }
        const auto inertial = transform.rotation.inverse_position(part);
        for (unsigned i = 0; i < 3; ++i)
            acceleration[i] += inertial[i];
    }
    if constexpr (Differential) {
        finite(acceleration);
        finite(result.drag_parameter_jacobian_pa);
        finite(result.srp_parameter_jacobian_pa);
        for (unsigned i = 0; i < 3; ++i) {
            finite(result.force.position_jacobian_per_s2[i]);
            finite(result.force.velocity_jacobian_per_s[i]);
        }
        check();
    }
    return result;
}
} // namespace

void detail::prepare_hpop_force_environment(const CartesianState &state, const HpopOptions &options,
                                            HpopEnvironment &environment) {
    validate_hpop_options(options);
    environment.validate_ephemeris_interval(options, state.epoch, state.epoch);
    if (options.gravity_model.empty())
        environment.gravity = builtin_egm96();
    if (!environment.gravity)
        throw std::invalid_argument(
            "Selected gravity model is missing; download or import it in Data Center");
    if (options.uses_secular_gravity() && !environment.gravity->info().secular_variations)
        throw std::invalid_argument(
            "Selected gravity file has no secular reference epoch and rates");
    if (environment.gravity->info().mu != options.gravity_mu)
        throw std::invalid_argument(
            "HPOP gravity GM differs from the selected file; update model settings");
    if (state.origin != OriginId::earth || state.frame != FrameId::gcrf)
        throw std::invalid_argument("HPOP requires an Earth-centered GCRF initial state");
    if (options.uses_eop_force() && (!environment.eop || !environment.frames))
        throw std::invalid_argument("HPOP gravity corrections, drag and relativity require "
                                    "explicit EOP and frame providers");
    if (options.uses_eop_force() && !eop_covers(environment.eop->data_info(), state.epoch))
        throw EopCoverageError("HPOP initial epoch is outside selected EOP coverage");
    if (options.uses_utc_force()) {
        if (!environment.time ||
            environment.time->data_info().sha256 != environment.eop->data_info().time_data.sha256)
            throw std::invalid_argument("HPOP forces require the EOP's confirmed UTC provider");
        (void)environment.time->to_utc(state.epoch);
    }
    if (options.ocean_tides.active() && !environment.ocean_tides)
        throw std::invalid_argument(
            "Selected ocean tide model is missing; download or import it in Data Center");
    environment.validate_weather_interval(options.drag, state.epoch, state.epoch);
}
void detail::validate_hpop_force_state(const Vector &position, const Vector &velocity,
                                       const HpopOptions &options,
                                       const HpopEnvironment &environment) {
    for (unsigned i = 0; i < 3; ++i)
        if (!std::isfinite(position[i]) || !std::isfinite(velocity[i]))
            throw PropagationError("HPOP encountered a nonfinite state");
    if (std::hypot(position[0], position[1], position[2]) <=
        environment.gravity->info().radius_m + options.minimum_altitude_m)
        throw PropagationError("HPOP reached its minimum geocentric altitude");
}
Vector detail::hpop_acceleration_unchecked(const CartesianState &state, const HpopOptions &options,
                                           const HpopEnvironment &environment,
                                           std::optional<Epoch> weather_at,
                                           std::optional<double> shadow_lighting) {
    return evaluate_force<false>(state, options, environment, weather_at, shadow_lighting);
}
Vector evaluate_hpop_acceleration(const CartesianState &state, const HpopOptions &options,
                                  HpopEnvironment environment) {
    detail::prepare_hpop_force_environment(state, options, environment);
    detail::validate_hpop_force_state(state.position_m, state.velocity_mps, options, environment);
    const auto result = detail::hpop_acceleration_unchecked(state, options, environment);
    finite(result);
    return result;
}
HpopForceDifferential hpop_force_differential(const CartesianState &state,
                                              const HpopOptions &options,
                                              HpopEnvironment environment,
                                              const std::function<void()> &checkpoint) {
    if (checkpoint)
        checkpoint();
    detail::prepare_hpop_force_environment(state, options, environment);
    detail::validate_hpop_force_state(state.position_m, state.velocity_mps, options, environment);
    return evaluate_force<true>(state, options, environment, {}, {}, checkpoint);
}
} // namespace orbitforge::core
