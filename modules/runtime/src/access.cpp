#include <algorithm>
#include <orbitforge/runtime/access.hpp>
#include <orbitforge/runtime/project.hpp>

namespace orbitforge::runtime {
namespace {
struct Pair {
    application::ProjectGroundStation station;
    application::ProjectSatellite satellite;
    std::optional<application::ProjectSensor> sensor;
    core::GroundStationLocation pointing_target;
};
Pair resolve(const application::Project &project, const std::vector<std::string> &ids) {
    if (ids.size() != 2)
        throw std::invalid_argument(
            "Access requires one ground station and one satellite or sensor");
    Pair pair;
    const auto station = std::ranges::find(project.ground_stations, ids[0],
                                           &application::ProjectGroundStation::object_id);
    if (station == project.ground_stations.end())
        throw std::invalid_argument("Access ground station is missing");
    pair.station = *station;
    std::string parent = ids[1];
    const auto sensor =
        std::ranges::find(project.sensors, ids[1], &application::ProjectSensor::object_id);
    if (sensor != project.sensors.end()) {
        pair.sensor = *sensor;
        parent = sensor->parent_satellite_id;
        if (sensor->pointing == application::SensorPointing::targeted) {
            const auto target =
                std::ranges::find(project.ground_stations, sensor->target_station_id,
                                  &application::ProjectGroundStation::object_id);
            if (target == project.ground_stations.end())
                throw std::invalid_argument("Sensor pointing target is missing");
            pair.pointing_target = target->location;
        }
    }
    const auto satellite = std::ranges::find_if(
        project.satellites, [&](const auto &s) { return s.orbit.object_id == parent; });
    if (satellite == project.satellites.end())
        throw std::invalid_argument("Access satellite or sensor is missing");
    pair.satellite = *satellite;
    return pair;
}
} // namespace
void validate_access_pair(const application::Project &project,
                          const std::vector<std::string> &ids) {
    (void)resolve(project, ids);
}
struct AccessSession::Impl {
    ReportContext context;
    Pair pair;
    application::Session orbit;
    Impl(ReportContext c, Pair p)
        : context(std::move(c)), pair(std::move(p)),
          orbit(make_orbit_session(pair.satellite.orbit, 1, context.time.get())) {}
};
AccessSession::AccessSession(ReportContext context, std::string station, std::string target) {
    application::validate_project(context.project);
    if (!context.time || !context.eop || !context.frames || !context.project.earth_orientation)
        throw std::invalid_argument("Access requires confirmed time, EOP and frame providers");
    if (!application::matching_project_data(*context.project.earth_orientation,
                                            context.eop->data_info()) ||
        context.time->data_info().sha256 != context.eop->data_info().time_data.sha256)
        throw std::invalid_argument("Access providers differ from the confirmed project data");
    auto pair = resolve(context.project, {station, target});
    impl_ = std::make_shared<Impl>(std::move(context), std::move(pair));
}
AccessSample AccessSession::at(core::Epoch epoch) const {
    const auto &c = impl_->context;
    if (epoch.tai_nanoseconds() < c.project.first.tai_nanoseconds() ||
        epoch.tai_nanoseconds() > c.project.last.tai_nanoseconds())
        throw std::out_of_range("Access epoch is outside the scenario");
    const std::array epochs{epoch};
    const auto state = core::transform_state(impl_->orbit.sample(epochs).states().front(),
                                             core::FrameId::itrf, *c.frames, c.eop->at(epoch));
    const auto &pair = impl_->pair;
    const auto observation = core::observe_ground_target(pair.station.location, state);
    bool visible = observation.line_of_sight &&
                   observation.elevation_rad >= pair.station.minimum_elevation_rad;
    std::optional<double> off_boresight;
    if (pair.sensor) {
        std::array<double, 3> axis{}, direction{};
        const auto station = core::ground_station_position(pair.station.location);
        const auto target = core::ground_station_position(pair.pointing_target);
        for (std::size_t i = 0; i < 3; ++i) {
            axis[i] =
                (pair.sensor->pointing == application::SensorPointing::targeted ? target[i] : 0) -
                state.position_m[i];
            direction[i] = station[i] - state.position_m[i];
        }
        off_boresight = core::vector_angle(axis, direction);
        visible = visible && *off_boresight <= pair.sensor->half_angle_rad;
    }
    return {epoch, observation, off_boresight, visible};
}
std::string AccessSession::name() const {
    return impl_->pair.station.name() + " → " +
           (impl_->pair.sensor ? impl_->pair.sensor->name() : impl_->pair.satellite.name());
}
} // namespace orbitforge::runtime
