#include "project_scene.hpp"
#include "project_update.hpp"
#include "satellite_model.hpp"
#include <algorithm>
#include <limits>
#include <orbelle/runtime/ephemeris.hpp>
#include <orbelle/runtime/gravity.hpp>
#include <orbelle/runtime/ocean_tides.hpp>
#include <orbelle/runtime/space_weather.hpp>

namespace orbelle::desktop {
std::shared_ptr<PreparedProject> prepare_project_scene(
    application::Project project, const std::filesystem::path &path,
    std::shared_ptr<const core::IFrameProvider> frames,
    const std::vector<std::shared_ptr<const core::IEarthOrientationProvider>> &installed_data,
    const std::atomic_bool &cancelled, std::optional<application::Scenario> retained_sessions,
    const std::optional<std::string> &edited_object,
    std::shared_ptr<const core::ITimeProvider> time,
    const std::map<std::string, std::shared_ptr<const core::GravityModel>> &gravity,
    const std::map<std::string, std::shared_ptr<const core::IThirdBodyEphemeris>> &ephemerides,
    const std::map<std::string, std::shared_ptr<const core::SpaceWeather>> &weather,
    const std::map<std::string, std::shared_ptr<const core::OceanTideModel>> &ocean_tides,
    const std::optional<application::Project> &previous_project,
    std::shared_ptr<runtime::OrbitSessionCache> sessions,
    const std::function<void(std::size_t)> &orbit_progress) {
    application::validate_project(project);
    const auto [data_first, data_last] = application::calculation_data_interval(project);
    std::shared_ptr<const core::IEarthOrientationProvider> eop;
    for (const auto &candidate : installed_data) {
        if (!candidate)
            continue;
        const auto &info = candidate->data_info();
        if (core::eop_covers(info, data_first) && core::eop_covers(info, data_last)) {
            eop = candidate;
            break;
        }
    }
    if (!eop)
        throw application::ProjectError(
            "No current EOP data covers this project's analysis interval");
    project.earth_orientation = eop->data_info();
    if (retained_sessions) {
        retained_sessions =
            retained_sessions->with_settings(project.name, project.first, project.last);
        // Keep surviving numerical sessions; rebuild every display with the same current EOP.
        const auto previous = *retained_sessions;
        for (const auto &session : previous.objects())
            if (std::none_of(project.satellites.begin(), project.satellites.end(),
                             [&](const auto &s) {
                                 return s.orbit.object_id == session.provenance().object_id;
                             }) &&
                std::none_of(
                    project.ground_stations.begin(), project.ground_stations.end(),
                    [&](const auto &s) { return s.object_id == session.provenance().object_id; }))
                retained_sessions =
                    retained_sessions->without_object(session.provenance().object_id);
    }
    auto result = std::make_shared<PreparedProject>(
        PreparedProject{project,
                        retained_sessions.value_or(
                            application::Scenario(project.name, project.first, project.last)),
                        eop,
                        {},
                        {},
                        {}});
    result->scenario = result->scenario.with_data_lock(project.data_lock());
    result->gravity = gravity;
    result->ephemerides = ephemerides;
    result->weather = weather;
    result->ocean_tides = ocean_tides;
    result->sessions =
        sessions ? std::move(sessions) : std::make_shared<runtime::OrbitSessionCache>();
    std::vector<std::string> active_ids;
    for (const auto &satellite : project.satellites)
        active_ids.push_back(satellite.orbit.object_id);
    result->sessions->retain_objects(active_ids);
    std::map<std::string, const application::Session *> existing_sessions;
    for (const auto &session : result->scenario.objects())
        existing_sessions.emplace(session.provenance().object_id, &session);
    std::vector<application::Session> updates;
    std::map<std::string, const application::OrbitInput *> previous_inputs;
    if (previous_project)
        for (const auto &satellite : previous_project->satellites)
            previous_inputs.emplace(satellite.orbit.object_id, &satellite.orbit);
    const auto previous_data =
        previous_project ? previous_project->data_lock() : application::DataLock{};
    const auto current_data = project.data_lock();
    updates.reserve(project.satellites.size() + project.ground_stations.size());
    result->orbits.reserve(project.satellites.size());
    result->models.reserve(project.satellites.size());
    for (auto &satellite : result->project.satellites) {
        if (cancelled.load())
            throw OrbitCancelled{};
        if (orbit_progress)
            orbit_progress(result->orbits.size());
        const auto &input = satellite.orbit;
        if (input.uses_hpop() && !input.hpop.gravity_model.empty() &&
            gravity.contains(input.hpop.gravity_model))
            runtime::validate_gravity_reference(project.force_data, input.hpop.gravity_model,
                                                gravity.at(input.hpop.gravity_model)->info());
        if (input.uses_hpop() && input.hpop_resource_options().uses_ephemeris() &&
            ephemerides.contains(input.hpop.ephemeris_model))
            runtime::validate_ephemeris_reference(
                project.force_data, input.hpop.ephemeris_model,
                ephemerides.at(input.hpop.ephemeris_model)->data_info());
        if (input.uses_hpop() && input.hpop.drag.enabled &&
            !input.hpop.drag.weather_model.empty() &&
            weather.contains(input.hpop.drag.weather_model))
            runtime::validate_space_weather_reference(
                project.force_data, input.hpop.drag.weather_model,
                weather.at(input.hpop.drag.weather_model)->info());
        if (input.uses_hpop() && input.hpop.ocean_tides.active() &&
            ocean_tides.contains(input.hpop.ocean_tides.model))
            runtime::validate_ocean_tide_reference(
                project.force_data, input.hpop.ocean_tides.model,
                ocean_tides.at(input.hpop.ocean_tides.model)->info());
        const auto found = existing_sessions.find(satellite.orbit.object_id);
        const auto *existing = found == existing_sessions.end() ? nullptr : found->second;
        const bool retained = existing != nullptr;
        const auto previous = previous_inputs.find(input.object_id);
        const bool recompute =
            !retained || previous == previous_inputs.end() ||
            !can_reuse_orbit_session(*previous->second, previous_data, input, current_data);
        if (retained && recompute &&
            existing->provenance().input_revision == std::numeric_limits<std::uint64_t>::max())
            throw std::overflow_error("Object input revision exhausted");
        auto session =
            retained && !recompute
                ? *existing
                : result->sessions->acquire(
                      satellite.orbit, retained ? existing->provenance().input_revision + 1 : 1,
                      time.get(),
                      {frames, eop,
                       gravity.contains(satellite.orbit.hpop.gravity_model)
                           ? gravity.at(satellite.orbit.hpop.gravity_model)
                           : nullptr,
                       ephemerides.contains(satellite.orbit.hpop.ephemeris_model)
                           ? ephemerides.at(satellite.orbit.hpop.ephemeris_model)
                           : nullptr,
                       time,
                       weather.contains(input.hpop.drag.weather_model)
                           ? weather.at(input.hpop.drag.weather_model)
                           : nullptr,
                       ocean_tides.contains(input.hpop.ocean_tides.model)
                           ? ocean_tides.at(input.hpop.ocean_tides.model)
                           : nullptr});
        auto orbit = prepare_orbit_scene(
            satellite.orbit, session, *frames, *eop, cancelled,
            display_cache_interval(satellite.orbit, satellite.display, project.current));
        vsg::ref_ptr<vsg::MatrixTransform> model;
        if (satellite.model_kind != 0) {
            std::filesystem::path model_path;
            if (satellite.model_kind == 2) {
                model_path = std::filesystem::path(
                    std::u8string(satellite.model_path.begin(), satellite.model_path.end()));
                if (model_path.is_relative())
                    model_path = path.parent_path() / model_path;
                model_path = std::filesystem::absolute(model_path).lexically_normal();
                const auto utf8 = model_path.generic_u8string();
                satellite.model_path.assign(reinterpret_cast<const char *>(utf8.data()),
                                            utf8.size());
            }
            model = vsg::MatrixTransform::create();
            model->addChild(make_satellite_model(model_path));
        }
        if (!retained || recompute)
            updates.push_back(std::move(session));
        result->orbits.push_back(std::move(orbit));
        result->models.push_back(std::move(model));
    }
    for (const auto &station : result->project.ground_stations) {
        if (cancelled.load())
            throw OrbitCancelled{};
        const auto found = existing_sessions.find(station.object_id);
        const auto *existing = found == existing_sessions.end() ? nullptr : found->second;
        const bool retained = existing != nullptr;
        const bool recompute = edited_object && *edited_object == station.object_id;
        if (retained && recompute &&
            existing->provenance().input_revision == std::numeric_limits<std::uint64_t>::max())
            throw std::overflow_error("Object input revision exhausted");
        if (!retained || recompute)
            updates.push_back(runtime::make_ground_station_session(
                station, retained ? existing->provenance().input_revision + 1 : 1));
    }
    if (cancelled.load())
        throw OrbitCancelled{};
    // Existing pointers refer to the old scenario only; all reads precede this replacement.
    result->scenario = result->scenario.with_objects(std::move(updates));
    result->ground_tracks = GroundMap::prepare_tracks(result->orbits);
    for (std::size_t i = 0; i < result->ground_tracks.size(); ++i)
        result->ground_tracks[i].name =
            QString::fromStdString(result->project.satellites[i].name());
    if (cancelled.load())
        throw OrbitCancelled{};
    return result;
}
} // namespace orbelle::desktop
