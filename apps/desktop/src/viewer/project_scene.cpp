#include "project_scene.hpp"
#include "satellite_model.hpp"
#include <algorithm>
#include <limits>

namespace orbitforge::desktop {
std::shared_ptr<PreparedProject> prepare_project_scene(
    application::Project project, const std::filesystem::path &path,
    const core::IFrameProvider &frames,
    const std::vector<std::shared_ptr<const core::IEarthOrientationProvider>> &installed_data,
    const std::atomic_bool &cancelled, std::optional<application::Scenario> retained_sessions,
    const std::optional<std::string> &edited_object) {
    application::validate_project(project);
    std::shared_ptr<const core::IEarthOrientationProvider> eop;
    for (const auto &candidate : installed_data) {
        if (!candidate)
            continue;
        const auto &info = candidate->data_info();
        if (core::eop_covers(info, project.first) && core::eop_covers(info, project.last)) {
            eop = candidate;
            break;
        }
    }
    if (!eop)
        throw application::ProjectError(
            "No current EOP data covers this project's analysis interval");
    project.earth_orientation = eop->data_info();
    if (retained_sessions) {
        // Keep surviving numerical sessions; rebuild every display with the same current EOP.
        const auto previous = *retained_sessions;
        for (const auto &session : previous.objects())
            if (std::none_of(project.satellites.begin(), project.satellites.end(),
                             [&](const auto &s) {
                                 return s.orbit.object_id == session.provenance().object_id;
                             }))
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
    for (auto &satellite : result->project.satellites) {
        if (cancelled.load())
            throw OrbitCancelled{};
        const auto existing = std::find_if(
            result->scenario.objects().begin(), result->scenario.objects().end(),
            [&](const auto &s) { return s.provenance().object_id == satellite.orbit.object_id; });
        const bool retained = existing != result->scenario.objects().end();
        const bool recompute = edited_object && *edited_object == satellite.orbit.object_id;
        if (retained && recompute &&
            existing->provenance().input_revision == std::numeric_limits<std::uint64_t>::max())
            throw std::overflow_error("Object input revision exhausted");
        auto session =
            retained && !recompute
                ? *existing
                : make_orbit_session(satellite.orbit,
                                     retained ? existing->provenance().input_revision + 1 : 1);
        auto orbit = prepare_orbit_scene(
            satellite.orbit, session, frames, *eop, cancelled,
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
            result->scenario = result->scenario.with_object(std::move(session));
        result->orbits.push_back(std::move(orbit));
        result->models.push_back(std::move(model));
    }
    result->ground_tracks = GroundMap::prepare_tracks(result->orbits);
    for (std::size_t i = 0; i < result->ground_tracks.size(); ++i)
        result->ground_tracks[i].name =
            QString::fromStdString(result->project.satellites[i].name());
    if (cancelled.load())
        throw OrbitCancelled{};
    return result;
}
} // namespace orbitforge::desktop
