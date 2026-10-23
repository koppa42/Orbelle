#include "access_geometry.hpp"
#include <algorithm>
#include <cmath>
#include <map>
#include <orbelle/runtime/access.hpp>
#include <orbelle/runtime/project.hpp>

namespace orbelle::runtime {
detail::AccessGeometry::AccessGeometry(const application::Project &project,
                                       const std::vector<std::string> &ids) {
    if (ids.size() != 2)
        throw std::invalid_argument(
            "Access requires one ground station and one satellite or sensor");
    const auto found_station = std::ranges::find(project.ground_stations, ids[0],
                                                 &application::ProjectGroundStation::object_id);
    if (found_station == project.ground_stations.end())
        throw std::invalid_argument("Access ground station is missing");
    station = *found_station;
    std::string parent = ids[1];
    const auto found_sensor =
        std::ranges::find(project.sensors, ids[1], &application::ProjectSensor::object_id);
    if (found_sensor != project.sensors.end()) {
        sensor = *found_sensor;
        pattern.emplace(sensor->pattern);
        parent = sensor->parent_satellite_id;
        if (sensor->pointing == application::SensorPointing::targeted) {
            const auto target =
                std::ranges::find(project.ground_stations, sensor->target_station_id,
                                  &application::ProjectGroundStation::object_id);
            if (target == project.ground_stations.end())
                throw std::invalid_argument("Sensor pointing target is missing");
            pointing_target = *target;
        }
    }
    const auto found_satellite = std::ranges::find_if(
        project.satellites, [&](const auto &s) { return s.orbit.object_id == parent; });
    if (found_satellite == project.satellites.end())
        throw std::invalid_argument("Access satellite or sensor is missing");
    satellite = *found_satellite;
}
AccessSample detail::AccessGeometry::at(const core::CartesianState &state) const {
    const auto observation = core::observe_ground_target(station.location, state);
    bool visible =
        observation.line_of_sight && observation.elevation_rad >= station.minimum_elevation_rad;
    std::optional<double> off_boresight;
    std::optional<double> sensor_margin;
    if (sensor) {
        std::array<double, 3> axis{}, direction{}, target_position{};
        const auto station_position = core::ground_station_position(station.location);
        if (pointing_target)
            target_position = core::ground_station_position(pointing_target->location);
        for (std::size_t i = 0; i < 3; ++i) {
            axis[i] = target_position[i] - state.position_m[i];
            direction[i] = station_position[i] - state.position_m[i];
        }
        off_boresight = core::vector_angle(axis, direction);
        const auto axes = core::sensor_axes(state.velocity_mps, axis, sensor->roll_rad);
        sensor_margin = pattern->margin(axes.to_local(direction));
        if (std::abs(*sensor_margin) <= 1e-14)
            sensor_margin = 0;
        visible = visible && *sensor_margin >= 0;
    }
    return {state.epoch, observation, off_boresight, visible, sensor_margin};
}
std::string detail::AccessGeometry::target_name() const {
    return sensor ? sensor->name() : satellite.name();
}
application::Project detail::AccessGeometry::numerical_inputs(core::Epoch first,
                                                              core::Epoch last) const {
    application::Project inputs{"analysis-access-input-v1", first, last};
    inputs.satellites.push_back(satellite);
    inputs.ground_stations.push_back(station);
    if (sensor)
        inputs.sensors.push_back(*sensor);
    if (pointing_target && pointing_target->object_id != station.object_id) {
        inputs.ground_stations.push_back(*pointing_target);
        // Pointing uses the other station's position, not its elevation constraint.
        inputs.ground_stations.back().minimum_elevation_rad = 0;
    }
    return inputs;
}
void validate_access_pair(const application::Project &project,
                          const std::vector<std::string> &ids) {
    (void)detail::AccessGeometry{project, ids};
}
std::vector<results::AccessWindow>
calculate_access_passes(const application::Project &project, const std::vector<std::string> &ids,
                        const results::AccessQuery &query,
                        const std::function<AccessSample(core::Epoch)> &at) {
    const detail::AccessGeometry geometry(project, ids);
    if (query.search_mode != core::AccessSearchMode::fixed_grid &&
        query.search_mode != core::AccessSearchMode::extremum_assisted)
        throw std::invalid_argument("Invalid Access search mode");
    std::map<std::int64_t, AccessSample> cache;
    const auto sample = [&](core::Epoch t) -> const AccessSample & {
        const auto key = t.tai_nanoseconds();
        if (const auto found = cache.find(key); found != cache.end())
            return found->second;
        if (cache.size() >= 2'000'000)
            throw std::invalid_argument(
                "Pass analysis exceeds 2000000 evaluations; shorten the interval");
        return cache.emplace(key, at(t)).first->second;
    };
    const auto step = query.search_step.count(), tolerance = query.boundary_tolerance.count();
    const auto intervals = query.search_mode == core::AccessSearchMode::fixed_grid
                               ? core::find_access_intervals(
                                     query.first, query.last, step,
                                     [&](auto t) { return sample(t).visible; }, tolerance)
                               : core::find_constraint_intervals(
                                     query.first, query.last, step,
                                     [&](auto t) {
                                         const auto &s = sample(t);
                                         auto margin = s.observation.elevation_rad -
                                                       geometry.station.minimum_elevation_rad;
                                         if (geometry.sensor)
                                             margin = std::min(margin, s.sensor_margin_rad.value());
                                         // Negative elevations from a raised observer must also
                                         // clear the station's local ground ellipsoid. Preserve
                                         // the separate segment test near its tangent boundary.
                                         if (!s.observation.line_of_sight)
                                             margin = std::min(margin, -1e-14);
                                         return margin;
                                     },
                                     tolerance);
    std::vector<results::AccessWindow> result;
    for (const auto &w : intervals) {
        const auto elevation = core::find_access_extremum(
            w.first, w.last, step, [&](auto t) { return sample(t).observation.elevation_rad; },
            true, tolerance);
        const auto range = core::find_access_extremum(
            w.first, w.last, step, [&](auto t) { return sample(t).observation.range_m; }, false,
            tolerance);
        result.emplace_back(w.first, w.last, w.start_clipped, w.stop_clipped,
                            core::PassExtrema{elevation, range});
    }
    return result;
}
struct AccessSession::Impl {
    ReportContext context;
    detail::AccessGeometry pair;
    application::Session orbit;
    Impl(ReportContext c, detail::AccessGeometry p)
        : context(std::move(c)), pair(std::move(p)),
          orbit(make_orbit_session(pair.satellite.orbit, 1, context.time.get(),
                                   context.force_environment(pair.satellite.orbit))) {}
};
AccessSession::AccessSession(ReportContext context, std::string station, std::string target) {
    application::validate_project(context.project);
    if (!context.time || !context.eop || !context.frames || !context.project.earth_orientation)
        throw std::invalid_argument("Access requires confirmed time, EOP and frame providers");
    if (!application::matching_project_data(*context.project.earth_orientation,
                                            context.eop->data_info()) ||
        context.time->data_info().sha256 != context.eop->data_info().time_data.sha256)
        throw std::invalid_argument("Access providers differ from the confirmed project data");
    auto pair = detail::AccessGeometry{context.project, {station, target}};
    impl_ = std::make_shared<Impl>(std::move(context), std::move(pair));
}
AccessSample AccessSession::at(core::Epoch epoch, const std::function<void()> &checkpoint) const {
    const auto &c = impl_->context;
    if (epoch.tai_nanoseconds() < c.project.first.tai_nanoseconds() ||
        epoch.tai_nanoseconds() > c.project.last.tai_nanoseconds())
        throw std::out_of_range("Access epoch is outside the scenario");
    const std::array epochs{epoch};
    const auto state =
        core::transform_state(impl_->orbit.sample(epochs, checkpoint).states().front(),
                              core::FrameId::itrf, *c.frames, c.eop->at(epoch));
    return impl_->pair.at(state);
}
std::string AccessSession::name() const {
    return impl_->pair.station.name() + " → " + impl_->pair.target_name();
}
} // namespace orbelle::runtime
