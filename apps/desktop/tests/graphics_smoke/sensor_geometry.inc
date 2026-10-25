void Workbench::smoke_sensor_geometry() {
    switch (orbit_smoke_stage_) {
    case 204: {
        auto project = project_snapshot(project_path_);
        project.sensors.clear();
        application::ProjectSensor sensor;
        sensor.object_id = "geometry-smoke";
        sensor.display_name = "Geometry sensor";
        sensor.parent_satellite_id = project.satellites.front().orbit.object_id;
        sensor.tilt_x_rad = .2;
        sensor.tilt_y_rad = -.1;
        sensor.roll_rad = .35;
        sensor.pattern.custom_points = {{.4, 0},
                                        {.25, std::numbers::pi / 2},
                                        {.4, std::numbers::pi},
                                        {.25, 3 * std::numbers::pi / 2}};
        sensor.pattern.inner_half_angle_rad = .15;
        sensor.pattern.minimum_clock_rad = 5 * std::numbers::pi / 3;
        sensor.pattern.maximum_clock_rad = std::numbers::pi / 3;
        project.sensors.push_back(sensor);
        start_project(std::move(project), project_path_, true);
        smoke_final_frames_ = 0;
        ++orbit_smoke_stage_;
        break;
    }
    case 205: {
        if (project_candidate_.valid() || ++smoke_final_frames_ < 4)
            break;
        smoke_require(sensor_layer_ && sensor_views_.contains("geometry-smoke"),
                      "Sensor volume was not published");
        const auto type = static_cast<int>(sensors_.front().pattern.type);
        save_workbench_image(QString("sensor-geometry-%1.png").arg(type));
        workspace_->select_view(1);
        save_workbench_image(QString("sensor-footprint-%1.png").arg(type));
        workspace_->select_view(0);
        save_project_to(smoke_dir_ + QString("/sensor-geometry-%1.orbelle").arg(type));
        smoke_record(
            QString("sensor-geometry-%1-rendered-and-saved").arg(type).toStdString().c_str());
        if (type == 3) {
            ++orbit_smoke_stage_;
            smoke_final_frames_ = 0;
            break;
        }
        auto project = project_snapshot(project_path_);
        project.sensors.front().pattern.type = static_cast<core::SensorPatternType>(type + 1);
        start_project(std::move(project), project_path_, true);
        smoke_final_frames_ = 0;
        break;
    }
    case 206: {
        auto project = project_snapshot(project_path_);
        const auto state = satellites_.front().orbit->track.sample_state(snapshot_.epoch);
        const vsg::dvec3 scaled(state.position_m[0] / 6378137., state.position_m[1] / 6378137.,
                                state.position_m[2] / 6356752.314245179);
        const auto radial = vsg::normalize(scaled);
        const auto axes = core::sensor_axes(state.velocity_mps, state.position_m);
        const auto along = vsg::normalize(vsg::dvec3(axes.x[0], axes.x[1], axes.x[2]));
        const auto angle = .8 * std::acos(1 / vsg::length(scaled));
        const auto target = vsg::normalize(radial * std::cos(angle) + along * std::sin(angle));
        application::ProjectGroundStation station;
        station.object_id = "limb-station";
        station.display_name = "Limb station";
        station.location = core::ground_station_location(
            core::GroundPositionType::cartesian,
            {target.x * 6378137., target.y * 6378137., target.z * 6356752.314245179});
        project.ground_stations.push_back(station);
        auto &sensor = project.sensors.front();
        sensor.pattern = core::SensorPattern(45 * std::numbers::pi / 180);
        sensor.pointing = application::SensorPointing::targeted;
        sensor.target_station_id = station.object_id;
        sensor.tilt_x_rad = sensor.tilt_y_rad = sensor.roll_rad = 0;
        sensor.display.color = 0x4eb3ff;
        start_project(std::move(project), project_path_, true);
        smoke_final_frames_ = 0;
        ++orbit_smoke_stage_;
        break;
    }
    case 207: {
        if (project_candidate_.valid() || ++smoke_final_frames_ < 4)
            break;
        change_display_frame(1);
        lighting_action_->setChecked(false);
        smoke_set_view_layout(0);
        set_selection(0);
        const auto state = satellites_.front().orbit->track.sample_state(snapshot_.epoch);
        const auto axes = core::sensor_axes(state.velocity_mps, state.position_m);
        const vsg::dvec3 satellite(state.position_m[0], state.position_m[1], state.position_m[2]);
        const vsg::dvec3 radial(axes.z[0], axes.z[1], axes.z[2]);
        const vsg::dvec3 along(axes.x[0], axes.x[1], axes.x[2]);
        const vsg::dvec3 side(axes.y[0], axes.y[1], axes.y[2]);
        const auto center = satellite + along * 1e6 - radial * 3e5;
        slots_[0]->navigation->set_viewpoint(
            vsg::LookAt::create(center + radial * 4e6 + side * 4e6, center, radial));
        smoke_final_frames_ = 0;
        ++orbit_smoke_stage_;
        break;
    }
    case 208:
        if (++smoke_final_frames_ < 4 || !view_captures_ready())
            break;
        save_workbench_image("sensor-station-limb-3d.png");
        save_project_to(smoke_dir_ + "/sensor-station-limb.orbelle");
        workspace_->select_view(1);
        ground_map_->reset_view();
        ground_map_->focus_selection();
        ground_map_->zoom_centered(2);
        smoke_final_frames_ = 0;
        ++orbit_smoke_stage_;
        break;
    case 209: {
        if (++smoke_final_frames_ < 4 || !ground_map_->footprints_ready())
            break;
        save_workbench_image("sensor-station-limb-2d.png");
        smoke_record("sensor-station-tracking-zero-offset-limb-rendered");
        ground_map_->reset_view();
        workspace_->select_view(0);
        auto project = project_snapshot(project_path_);
        project.sensors.front().display.show_space_extension = true;
        start_project(std::move(project), project_path_, true);
        smoke_final_frames_ = 0;
        ++orbit_smoke_stage_;
        break;
    }
    case 210: {
        if (project_candidate_.valid() || ++smoke_final_frames_ < 4 || !view_captures_ready())
            break;
        save_workbench_image("sensor-station-limb-space-3d.png");
        const auto extension_saved = smoke_dir_ + "/sensor-station-limb-space.orbelle";
        save_project_to(extension_saved);
        smoke_require(
            application::load_project(std::filesystem::path(extension_saved.toStdWString()))
                .sensors.front()
                .display.show_space_extension,
            "Sensor space extension setting was not saved");
        smoke_record("sensor-space-extension-enabled-and-saved");
        // Restore the bounded FOV before the general whole-globe framing check.
        // The targeted case intentionally includes a long extension into space.
        const auto saved = smoke_dir_ + "/sensor-geometry-3.orbelle";
        start_project(application::load_project(std::filesystem::path(saved.toStdWString())), saved,
                      true);
        fit_views();
        smoke_final_frames_ = 0;
        ++orbit_smoke_stage_;
        break;
    }
    case 211: {
        if (project_candidate_.valid() || ++smoke_final_frames_ < 4)
            break;
        auto project = project_snapshot(project_path_);
        auto &sensor = project.sensors.front();
        sensor.pattern = core::SensorPattern(45 * std::numbers::pi / 180);
        sensor.pointing = application::SensorPointing::nadir;
        sensor.target_station_id.clear();
        sensor.tilt_x_rad = sensor.tilt_y_rad = sensor.roll_rad = 0;
        sensor.display.color = 0xff00ff;
        sensor.display.opacity = .85;
        start_project(std::move(project), project_path_, true);
        smoke_final_frames_ = 0;
        ++orbit_smoke_stage_;
        break;
    }
    case 212:
        if (project_candidate_.valid() || ++smoke_final_frames_ < 4)
            break;
        workspace_->select_view(1);
        // Smoke playback uses frame_count_ seconds. Warm the map near that time
        // so the first moving sample remains inside the focused viewport.
        seek_epoch(core::Epoch{
            scenario_->first().tai_nanoseconds() +
            static_cast<std::int64_t>(
                std::fmod(static_cast<double>(frame_count_), duration_seconds()) * 1e9)});
        snapshot_ = sample_frame(*precise_epoch_);
        update_satellite();
        set_selection(0);
        ground_map_->reset_view();
        ground_map_->focus_selection();
        smoke_final_frames_ = 0;
        ++orbit_smoke_stage_;
        break;
    case 213:
        if (++smoke_final_frames_ < 4 || !ground_map_->footprints_ready())
            break;
        save_workbench_image("sensor-playback-start-2d.png");
        smoke_view_epoch_ = snapshot_.epoch;
        set_playing(true);
        smoke_final_frames_ = 0;
        ++orbit_smoke_stage_;
        break;
    case 214: {
        ++smoke_final_frames_;
        if (smoke_final_frames_ == 6 || smoke_final_frames_ == 22)
            ground_map_->zoom_centered(2);
        if (smoke_final_frames_ == 14 || smoke_final_frames_ == 30)
            ground_map_->zoom_centered(.5);
        const auto image = ground_map_->grab().toImage();
        const auto ratio = image.devicePixelRatio();
        int coverage = 0;
        for (int y = qCeil(14 * ratio); y < image.height() - 44 * ratio; ++y)
            for (int x = qCeil(38 * ratio); x < image.width() - 16 * ratio; ++x) {
                const auto color = image.pixelColor(x, y);
                coverage += color.red() > 180 && color.blue() > 180 && color.green() < 70;
            }
        smoke_require(coverage > 20, "2D sensor coverage disappeared during playback/zoom");
        if (smoke_final_frames_ == 12)
            save_workbench_image("sensor-playback-zoom-2d.png");
        if (smoke_final_frames_ < 40)
            break;
        smoke_require(snapshot_.epoch.tai_nanoseconds() > smoke_view_epoch_.tai_nanoseconds() &&
                          ground_map_->epoch() == snapshot_.epoch,
                      "Sensor playback did not advance the shared scene time");
        set_playing(false);
        save_workbench_image("sensor-playback-stop-2d.png");
        smoke_record("sensor-2d-coverage-continuous-through-playback-and-zoom");
        const auto saved = smoke_dir_ + "/sensor-geometry-3.orbelle";
        start_project(application::load_project(std::filesystem::path(saved.toStdWString())), saved,
                      true);
        smoke_final_frames_ = 0;
        ++orbit_smoke_stage_;
        break;
    }
    case 215:
        if (project_candidate_.valid() || ++smoke_final_frames_ < 4)
            break;
        ground_map_->reset_view();
        workspace_->select_view(0);
        fit_views();
        ++orbit_smoke_stage_;
        break;
    default:
        throw std::logic_error("Stage does not belong to sensor_geometry");
    }
}
