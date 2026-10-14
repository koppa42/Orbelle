#include <algorithm>
#include <orbitforge/runtime/ephemeris.hpp>
#include <orbitforge/runtime/gravity.hpp>
#include <orbitforge/runtime/hpop.hpp>
#include <orbitforge/runtime/j2.hpp>
#include <orbitforge/runtime/j4.hpp>
#include <orbitforge/runtime/mission.hpp>
#include <orbitforge/runtime/project.hpp>
#include <orbitforge/runtime/sgp4.hpp>

namespace orbitforge::runtime {
application::Session make_ground_station_session(const application::ProjectGroundStation &station,
                                                 std::uint64_t revision) {
    return {std::make_shared<core::GroundStation>(station.location),
            {station.object_id, revision, "ground-station-wgs84-fixed-itrf-v1"}};
}
application::Session make_orbit_session(const application::OrbitInput &input,
                                        std::uint64_t revision, const core::ITimeProvider *time,
                                        core::HpopEnvironment environment) {
    const auto &body = core::central_body(input.initial.origin);
    if (input.uses_hpop() && input.hpop.uses_third_bodies()) {
        if (!environment.ephemeris)
            throw core::EphemerisDataError("Sun/Moon gravity requires an ephemeris");
        const auto first = core::Epoch{
            std::min(input.initial.epoch.tai_nanoseconds(), input.first.tai_nanoseconds())};
        const auto last = core::Epoch{
            std::max(input.initial.epoch.tai_nanoseconds(), input.last.tai_nanoseconds())};
        environment.ephemeris->validate_interval(first, last, input.hpop.sun_gravity,
                                                 input.hpop.moon_gravity);
    }
    if (input.uses_eop_force()) {
        if (!environment.eop || !environment.frames)
            throw std::invalid_argument("HPOP spherical gravity requires selected EOP data");
        for (auto epoch : {input.initial.epoch, input.first, input.last})
            if (!core::eop_covers(environment.eop->data_info(), epoch))
                throw core::EopCoverageError("HPOP needs EOP covering the entire integration "
                                             "interval, including the initial epoch");
    }
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
        return make_hpop_session(input.initial, input.hpop, std::move(environment), input.object_id,
                                 revision);
    case application::OrbitPropagator::mission:
        return make_mission_session(input.initial, input.mission, input.hpop,
                                    std::move(environment), input.object_id, revision);
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
    for (const auto &satellite : project.satellites) {
        const auto &input = satellite.orbit;
        if (input.uses_hpop() && input.hpop.uses_third_bodies()) {
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
        result = result.with_object(make_orbit_session(satellite.orbit, 1, time, environment));
    }
    for (const auto &station : project.ground_stations)
        result = result.with_object(make_ground_station_session(station));
    return result;
}
} // namespace orbitforge::runtime
