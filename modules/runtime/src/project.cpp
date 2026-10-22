#include <algorithm>
#include <orbelle/runtime/ephemeris.hpp>
#include <orbelle/runtime/gravity.hpp>
#include <orbelle/runtime/hpop.hpp>
#include <orbelle/runtime/j2.hpp>
#include <orbelle/runtime/j4.hpp>
#include <orbelle/runtime/mission.hpp>
#include <orbelle/runtime/ocean_tides.hpp>
#include <orbelle/runtime/project.hpp>
#include <orbelle/runtime/sgp4.hpp>
#include <orbelle/runtime/space_weather.hpp>

namespace orbelle::runtime {
application::Session make_ground_station_session(const application::ProjectGroundStation &station,
                                                 std::uint64_t revision) {
    return {std::make_shared<core::GroundStation>(station.location),
            {station.object_id, revision, "ground-station-wgs84-fixed-itrf-v1"}};
}
application::Session make_orbit_session(const application::OrbitInput &input,
                                        std::uint64_t revision, const core::ITimeProvider *time,
                                        core::HpopEnvironment environment) {
    const auto &body = core::central_body(input.initial.origin);
    if (input.uses_hpop() && input.hpop_resource_options().uses_ephemeris()) {
        if (!environment.ephemeris)
            throw core::EphemerisDataError("Enabled HPOP forces require an ephemeris");
        const auto first = core::Epoch{
            std::min(input.initial.epoch.tai_nanoseconds(), input.first.tai_nanoseconds())};
        const auto last = core::Epoch{
            std::max(input.initial.epoch.tai_nanoseconds(), input.last.tai_nanoseconds())};
        environment.validate_ephemeris_interval(input.hpop_resource_options(), first, last);
    }
    if (input.requires_eop()) {
        if (!environment.eop || !environment.frames)
            throw std::invalid_argument(
                "HPOP forces or covariance coordinates require selected EOP data");
        for (auto epoch : {input.initial.epoch, input.first, input.last})
            if (!core::eop_covers(environment.eop->data_info(), epoch))
                throw core::EopCoverageError("HPOP needs EOP covering the entire integration "
                                             "interval, including the initial epoch");
        if (input.hpop_resource_options().uses_utc_force()) {
            if (!environment.time)
                throw std::invalid_argument("HPOP forces require a retained UTC provider");
            for (auto epoch : {input.initial.epoch, input.first, input.last})
                (void)environment.time->to_utc(epoch);
        }
    }
    if (input.uses_hpop())
        environment.validate_weather_interval(
            input.hpop.drag,
            core::Epoch{
                std::min(input.initial.epoch.tai_nanoseconds(), input.first.tai_nanoseconds())},
            core::Epoch{
                std::max(input.initial.epoch.tai_nanoseconds(), input.last.tai_nanoseconds())});
    if (input.uses_hpop()) {
        application::validate_hpop_covariance_settings(input.covariance, input.hpop);
        if (input.covariance.enabled && !input.computes_covariance())
            throw std::invalid_argument("Covariance across mission maneuvers is not supported");
    }
    if (input.computes_covariance())
        (void)core::propagate_hpop_covariance(input.initial, input.initial.epoch,
                                              input.covariance.active_input(), input.hpop,
                                              environment, input.covariance.variational_options());
    switch (input.propagator) {
    case application::OrbitPropagator::two_body:
        return make_two_body_session(input.initial, body.mu, input.object_id, revision);
    case application::OrbitPropagator::j2_secular:
        return make_j2_session(j2_mean_ellipse_elements(input.initial), core::J2SecularOptions{},
                               input.object_id, revision, body.origin);
    case application::OrbitPropagator::j4_secular:
        return make_j4_session(j4_mean_ellipse_elements(input.initial), input.object_id, revision);
    case application::OrbitPropagator::sgp4:
        if (!time)
            throw std::invalid_argument(
                "SGP4 requires an explicit time provider for the TLE epoch");
        return make_sgp4_session(input.tle, *time, input.object_id, revision);
    case application::OrbitPropagator::hpop:
        return make_hpop_interpolated_session(
            input.initial, input.first, input.last, input.hpop, std::move(environment),
            input.hpop.report_ephemeris_on_fixed_step
                ? static_cast<std::int64_t>(input.step_seconds) * 1'000'000'000LL
                : 0,
            input.object_id, revision);
    case application::OrbitPropagator::mission:
        return make_mission_session(input.initial, input.mission, input.hpop,
                                    std::move(environment), input.object_id, revision,
                                    input.hpop.report_ephemeris_on_fixed_step
                                        ? static_cast<std::int64_t>(input.step_seconds) *
                                              1'000'000'000LL
                                        : 0,
                                    input.last);
    }
    throw std::invalid_argument("Unsupported orbit propagator");
}
application::Scenario make_project_scenario(const application::Project &project,
                                            const core::ITimeProvider *time,
                                            core::HpopEnvironment environment) {
    application::validate_project(project);
    const auto recorded_time = project.time_data_info();
    if (time && recorded_time &&
        application::time_data_lock(time->data_info()) !=
            application::time_data_lock(*recorded_time) &&
        std::any_of(project.satellites.begin(), project.satellites.end(),
                    [](const auto &satellite) {
                        return satellite.orbit.propagator == application::OrbitPropagator::sgp4;
                    }))
        throw application::ProjectError("SGP4 time data differs from the project's confirmed data");
    application::Scenario result{project.name, project.first, project.last};
    if (environment.eop && project.earth_orientation &&
        !application::matching_project_data(environment.eop->data_info(),
                                            *project.earth_orientation))
        throw application::ProjectError("HPOP EOP data differs from the project's confirmed data");
    result = result.with_data_lock(project.data_lock());
    std::vector<application::Session> sessions;
    sessions.reserve(project.satellites.size() + project.ground_stations.size());
    for (const auto &satellite : project.satellites) {
        const auto &input = satellite.orbit;
        if (input.uses_hpop() && input.hpop_resource_options().uses_ephemeris()) {
            if (!environment.ephemeris)
                throw application::ProjectError("Missing selected ephemeris");
            validate_ephemeris_reference(project.force_data, input.hpop.ephemeris_model,
                                         environment.ephemeris->data_info());
        }
        if (input.uses_hpop() && !input.hpop.gravity_model.empty()) {
            if (!environment.gravity)
                throw application::ProjectError("Missing selected gravity provider");
            validate_gravity_reference(project.force_data, input.hpop.gravity_model,
                                       environment.gravity->info());
        }
        if (input.uses_hpop() && input.hpop.drag.enabled &&
            !input.hpop.drag.weather_model.empty()) {
            if (!environment.weather)
                throw application::ProjectError("Missing space weather");
            validate_space_weather_reference(project.force_data, input.hpop.drag.weather_model,
                                             environment.weather->info());
        }
        if (input.uses_hpop() && input.hpop.ocean_tides.active()) {
            if (!environment.ocean_tides)
                throw application::ProjectError("Missing selected ocean tide model");
            validate_ocean_tide_reference(project.force_data, input.hpop.ocean_tides.model,
                                          environment.ocean_tides->info());
        }
        sessions.push_back(make_orbit_session(satellite.orbit, 1, time, environment));
    }
    for (const auto &station : project.ground_stations)
        sessions.push_back(make_ground_station_session(station));
    return result.with_objects(std::move(sessions));
}
} // namespace orbelle::runtime
