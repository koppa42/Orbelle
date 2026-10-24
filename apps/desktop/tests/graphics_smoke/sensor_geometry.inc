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
    case 206:
        if (++smoke_final_frames_ >= 4)
            ++orbit_smoke_stage_;
        break;
    default:
        throw std::logic_error("Stage does not belong to sensor_geometry");
    }
}
