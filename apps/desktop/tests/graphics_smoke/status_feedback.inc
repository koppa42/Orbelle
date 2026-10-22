void Workbench::smoke_status_feedback() {
    switch (orbit_smoke_stage_) {
    case 192: {
        auto project = application::load_project(
            std::filesystem::path((smoke_dir_ + "/apply-audit.orbelle").toStdWString()));
        project.ground_stations.push_back({"status-station", "Status station", {0, 0, 0}});
        start_project(std::move(project), smoke_dir_ + "/status-audit.orbelle");
        ++orbit_smoke_stage_;
        break;
    }
    case 193:
        set_selection(satellites_.size());
        open_properties();
        station_editor_->findChild<QuantityEditor *>("stationHeight")->set_value(10, Unit::metre);
        station_editor_->findChild<QPushButton *>("applyGroundStation")->click();
        smoke_require(project_candidate_.valid(), "Station Apply did not start a project update");
        project_cancellation_->store(true);
        ++orbit_smoke_stage_;
        break;
    case 194: {
        smoke_require(last_project_outcome_ == "cancelled" &&
                          stations_[0].config.location.height_m == 0 &&
                          station_editor_->findChild<QLabel *>("stationResult")->text() ==
                              QStringLiteral("已取消更新，原场景未改变。"),
                      "Cancelled station Apply was reported as a failure or stayed pending");
        smoke_record("station-cancel-publishes-cancellation-status");
        station_editor_->findChild<QPushButton *>("resetGroundStation")->click();
        station_editor_->show_result("Station feedback");
        set_selection(0);
        open_properties();
        smoke_cache_ = satellites_[0].orbit;
        editor_->set_state_mode(0);
        auto *velocity = editor_->findChild<QuantityEditor *>("velocityY");
        velocity->set_value(velocity->value(Unit::metres_per_second) + 1, Unit::metres_per_second);
        editor_->submit();
        smoke_require(project_candidate_.valid(), "HPOP Apply did not start a project update");
        editor_->cancel();
        ++orbit_smoke_stage_;
        break;
    }
    case 195:
        smoke_require(last_orbit_outcome_ == "cancelled" && satellites_[0].orbit == smoke_cache_ &&
                          editor_->has_unapplied_changes() &&
                          editor_->findChild<QLabel *>("orbitResult")->text() ==
                              QStringLiteral("已取消更新，原场景未改变。") &&
                          station_editor_->findChild<QLabel *>("stationResult")->text() ==
                              "Station feedback",
                      "HPOP cancellation lost its draft/status or overwrote another editor");
        smoke_record("hpop-cancel-preserves-draft-and-unrelated-feedback");
        editor_->submit();
        ++orbit_smoke_stage_;
        break;
    case 196:
        smoke_require(last_orbit_outcome_ == "applied" && !editor_->has_unapplied_changes() &&
                          editor_->findChild<QLabel *>("orbitResult")->text() ==
                              QStringLiteral("轨道已更新。"),
                      "HPOP retry retained cancellation or pending feedback");
        smoke_record("hpop-retry-replaces-cancellation-with-completion");
        workspace_->select_view(0);
        fit_views();
        fit_pending_ = true;
        smoke_final_frames_ = 0;
        ++orbit_smoke_stage_;
        break;
    case 197:
        if (++smoke_final_frames_ >= 4)
            ++orbit_smoke_stage_;
        break;
    default:
        throw std::logic_error("Stage does not belong to status_feedback");
    }
}
