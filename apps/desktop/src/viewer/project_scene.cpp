#include "project_scene.hpp"
#include "satellite_model.hpp"

namespace orbitforge::desktop {
std::shared_ptr<PreparedProject> prepare_project_scene(
    application::Project project, const std::filesystem::path &path,
    const core::IFrameProvider &frames,
    const std::vector<std::shared_ptr<const core::IEarthOrientationProvider>> &installed_data,
    const std::atomic_bool &cancelled) {
    application::validate_project(project);
    std::shared_ptr<const core::IEarthOrientationProvider> eop;
    for (const auto &candidate : installed_data) {
        if (!candidate)
            continue;
        const auto &info = candidate->data_info();
        if (info.first_epoch.tai_nanoseconds() <= project.first.tai_nanoseconds() &&
            info.last_epoch.tai_nanoseconds() >= project.last.tai_nanoseconds() &&
            (!project.earth_orientation ||
             application::matching_project_data(*project.earth_orientation, info))) {
            eop = candidate;
            break;
        }
    }
    if (!eop)
        throw application::ProjectError(
            project.earth_orientation
                ? "The project's exact EOP/time data version is not installed; current project "
                  "retained"
                : "No installed EOP data covers this project's analysis interval");
    project.earth_orientation = eop->data_info();
    auto result = std::make_shared<PreparedProject>(
        PreparedProject{project,
                        application::Scenario(project.name, project.first, project.last),
                        eop,
                        {},
                        {},
                        {}});
    for (auto &satellite : result->project.satellites) {
        if (cancelled.load())
            throw OrbitCancelled{};
        auto session = make_orbit_session(satellite.orbit);
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
        result->scenario = result->scenario.with_object(std::move(session));
        result->orbits.push_back(std::move(orbit));
        result->models.push_back(std::move(model));
    }
    result->ground_tracks = GroundMap::prepare_tracks(result->orbits);
    if (cancelled.load())
        throw OrbitCancelled{};
    return result;
}
} // namespace orbitforge::desktop
