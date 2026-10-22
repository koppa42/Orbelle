void Workbench::smoke_ground_stations() {
    auto input = orbit_ ? orbit_->input : OrbitInput{};
    switch (orbit_smoke_stage_) {
    case 103: {
        timer_.stop();
        show_insert_objects();
        insert_dialog_->select_type(InsertObjectType::ground_station);
        QApplication::processEvents();
        smoke_require(insert_dialog_->grab().save(smoke_dir_ + "/insert-ground-station.png"),
                      "Cannot capture station chooser");
        insert_dialog_->findChild<QPushButton *>("insertObject")->click();
        timer_.start();
        smoke_require(station_editor_ && workspace_->current_document() == station_editor_ &&
                          stations_.empty(),
                      "Station insertion must open an uncommitted property page");
        station_editor_->findChild<QLineEdit *>("stationName")
            ->setText(QStringLiteral("海岸观测站"));
        station_editor_->findChild<QuantityEditor *>("stationLongitude")
            ->set_value(114.2, Unit::degree);
        station_editor_->findChild<QuantityEditor *>("stationLatitude")
            ->set_value(22.3, Unit::degree);
        station_editor_->findChild<QuantityEditor *>("stationHeight")->set_value(-200, Unit::metre);
        station_editor_->findChild<QPushButton *>("applyGroundStation")->click();
        smoke_require(busy(), "Station creation did not start");
        smoke_record("ground-station-insert-via-object-chooser");
        ++orbit_smoke_stage_;
        break;
    }
    case 104: {
        orbit_capture_ = true;
        smoke_require(last_project_outcome_ == "opened" && stations_.size() == 1 &&
                          selected_station_ == 0 && !selected_ &&
                          stations_[0].config.location.height_m == -200 &&
                          scenario_->objects().size() == satellites_.size() + 1,
                      "Station creation did not publish the model and selection together");
        const auto index = satellites_.size();
        smoke_require(
            QLineF(ground_map_->marker_coordinates(index), QPointF(114.2, 22.3)).length() < 1e-8,
            "Station map location uses incorrect geodetic coordinates");
        change_display_frame(1);
        const auto displayed = snapshot_.markers.at(index).position_m;
        const auto exact = core::ground_station_position(stations_[0].config.location);
        double offset = 0;
        for (std::size_t i = 0; i < 3; ++i)
            offset += std::pow(displayed[i] - exact[i], 2);
        smoke_require(std::abs(std::sqrt(offset) - 300) < 1e-6,
                      "Station display clearance changed the numerical height");
        workspace_->select_view(0);
        focus_satellite(0);
        const auto back =
            -vsg::normalize(vsg::dvec3(displayed[0], displayed[1], displayed[2])) * 14e6;
        slots_[1]->navigation->set_viewpoint(
            vsg::LookAt::create(back, vsg::dvec3{}, vsg::dvec3(0, 0, 1)));
        set_selection(std::nullopt);
        smoke_final_frames_ = 0;
        smoke_record("ground-station-negative-height-keeps-exact-computation");
        ++orbit_smoke_stage_;
        break;
    }
    case 105: {
        if (++smoke_final_frames_ < 4)
            break;
        const auto &candidates = slots_[0]->pick_candidates;
        const auto found =
            std::find_if(candidates.begin(), candidates.end(),
                         [&](const auto &point) { return point.index == satellites_.size(); });
        smoke_require(found != candidates.end(),
                      "Front-facing ground station is not pickable in 3D");
        smoke_require(slots_[0]->station_markers->size() == 1 &&
                          slots_[1]->station_markers->size() == 0,
                      "Ground station annotations ignored per-view Earth occlusion");
        int teal = 0;
        const auto &pixels = slots_[0]->captured_image;
        smoke_require(pixels.save(smoke_dir_ + "/ground-station-glyph.png"),
                      "Cannot capture the station glyph");
        for (int y = static_cast<int>(found->y_pixels) - 5;
             y <= static_cast<int>(found->y_pixels) + 5; ++y)
            for (int x = static_cast<int>(found->x_pixels) - 5;
                 x <= static_cast<int>(found->x_pixels) + 5; ++x) {
                if (!pixels.rect().contains(x, y))
                    continue;
                const auto color = pixels.pixelColor(x, y);
                if (color.green() > 180 && color.blue() > 120 && color.red() < color.green() * 0.7)
                    ++teal;
            }
        smoke_require(teal >= 6, "Ground station glyph was not rendered above the surface");
        inject_click(*slots_[0], {found->x_pixels, found->y_pixels});
        smoke_require(selected_station_ == 0 && objects_->currentItem() == stations_[0].item,
                      "3D ground station pick selected a satellite");
        smoke_record("ground-station-visible-glyph-and-3d-pick");
        open_properties();
        smoke_project_bytes_ = application::encode_project(project_snapshot(project_path_));
        auto *latitude = station_editor_->findChild<QuantityEditor *>("stationLatitude");
        latitude->findChild<QLineEdit *>("quantityValue")->setText("95");
        station_editor_->findChild<QPushButton *>("applyGroundStation")->click();
        smoke_require(!busy() && smoke_project_bytes_ ==
                                     application::encode_project(project_snapshot(project_path_)),
                      "Invalid station latitude changed the scene");
        station_editor_->findChild<QPushButton *>("resetGroundStation")->click();
        const auto station_revision = scenario_->revision();
        station_editor_->findChild<QuantityEditor *>("stationLatitude")->set_unit(Unit::radian);
        station_editor_->findChild<QPushButton *>("applyGroundStation")->click();
        smoke_require(!busy() && scenario_->revision() == station_revision,
                      "Unchanged station Apply started work after a unit change");
        smoke_record("unchanged-ground-station-apply-preserves-scene");
        station_editor_->findChild<QuantityEditor *>("stationHeight")
            ->set_value(0.35, Unit::kilometre);
        station_editor_->findChild<QPushButton *>("applyGroundStation")->click();
        smoke_record("ground-station-invalid-input-reset-and-unit-edit");
        ++orbit_smoke_stage_;
        break;
    }
    case 106: {
        const auto found = std::find_if(
            scenario_->objects().begin(), scenario_->objects().end(), [&](const auto &session) {
                return session.provenance().object_id == stations_[0].config.object_id;
            });
        smoke_require(
            last_project_outcome_ == "opened" && stations_[0].config.location.height_m == 350 &&
                found != scenario_->objects().end() && found->provenance().input_revision == 2,
            "Editing station position failed to revise its numerical session");
        rename_selected_to(QStringLiteral("北岸地面站"));
        ++orbit_smoke_stage_;
        break;
    }
    case 107:
        smoke_require(last_project_outcome_ == "opened" &&
                          stations_[0].config.name() == "北岸地面站",
                      "Station rename failed");
        duplicate_action_->trigger();
        ++orbit_smoke_stage_;
        break;
    case 108:
        smoke_require(last_project_outcome_ == "opened" && stations_.size() == 2 &&
                          selected_station_ == 1 &&
                          stations_[0].config.object_id != stations_[1].config.object_id &&
                          stations_[0].config.location == stations_[1].config.location,
                      "Station copy did not preserve location and create a separate identity");
        smoke_confirm_delete(false);
        smoke_require(!busy() && stations_.size() == 2, "Cancelled deletion removed a station");
        smoke_confirm_delete(true);
        ++orbit_smoke_stage_;
        break;
    case 109:
        smoke_require(last_project_outcome_ == "opened" && stations_.size() == 1 &&
                          selected_station_ == 0,
                      "Station deletion lost the surviving station selection");
        smoke_record("ground-station-rename-copy-and-delete");
        save_project_to(smoke_dir_ + "/ground-stations.orbelle");
        start_project(
            application::load_project(std::filesystem::path(project_path_.toStdWString())),
            project_path_);
        ++orbit_smoke_stage_;
        break;
    case 110: {
        smoke_require(last_project_outcome_ == "opened" && stations_.size() == 1 &&
                          !selected_marker_index() &&
                          stations_[0].config.location.height_m == 350 &&
                          stations_[0].config.name() == "北岸地面站",
                      "Station project failed to reopen with its saved coordinates and name");
        auto *search = findChild<QLineEdit *>("objectSearch");
        search->setText(QStringLiteral("北岸"));
        smoke_require(!stations_[0].item->isHidden() && satellites_[0].item->isHidden(),
                      "Object search omitted ground stations");
        search->clear();
        workspace_->split_page(map_panel_, view_panel_, Qt::RightEdge);
        ground_map_->reset_view();
        const auto index = satellites_.size();
        const auto point = ground_map_->marker_position(index);
        const auto global = ground_map_->mapToGlobal(point);
        QMouseEvent press(QEvent::MouseButtonPress, point, global, Qt::LeftButton, Qt::LeftButton,
                          Qt::NoModifier);
        QMouseEvent release(QEvent::MouseButtonRelease, point, global, Qt::LeftButton, Qt::NoButton,
                            Qt::NoModifier);
        QCoreApplication::sendEvent(ground_map_, &press);
        QCoreApplication::sendEvent(ground_map_, &release);
        smoke_require(selected_station_ == 0 && objects_->currentItem() == stations_[0].item,
                      "2D station pick failed to synchronize the scene tree");
        const auto geographic = ground_map_->marker_coordinates(index);
        change_display_frame(0);
        seek_epoch(scenario_->first());
        snapshot_ = sample_frame(scenario_->first());
        update_satellite();
        const auto start = snapshot_.markers.at(index).position_m;
        seek_epoch(scenario_->last());
        snapshot_ = sample_frame(scenario_->last());
        update_satellite();
        smoke_require(start != snapshot_.markers.at(index).position_m &&
                          QLineF(geographic, ground_map_->marker_coordinates(index)).length() <
                              1e-8,
                      "Ground station did not rotate in GCRF while staying fixed on the map");
        focus_satellite();
        open_properties();
        smoke_record("ground-station-reopen-search-2d-pick-and-earth-rotation");
        smoke_final_frames_ = 0;
        ++orbit_smoke_stage_;
        break;
    }
    case 111: {
        if (++smoke_final_frames_ < 4)
            break;
        save_workbench_image("ground-station-properties.png");
        smoke_require(station_editor_->findChild<QPushButton *>("applyGroundStation")->isVisible(),
                      "Station apply button is hidden at this scale");
        station_editor_->close();
        const auto first = core::Epoch{scenario_->first().tai_nanoseconds() + 1'000'000'000};
        smoke_require(apply_scenario_settings({scenario_->name(), first, scenario_->last()}),
                      "Scenario interval edit rejected a mixed station project");
        ++orbit_smoke_stage_;
        break;
    }
    case 112: {
        smoke_require(last_project_outcome_ == "opened" && selected_station_ == 0 &&
                          stations_.size() == 1,
                      "Scenario interval edit lost its station or selection");
        save_workbench_image("ground-station-map-and-globe.png");
        auto only_stations = project_snapshot(project_path_);
        only_stations.satellites.clear();
        start_project(std::move(only_stations), project_path_, true, 0);
        ++orbit_smoke_stage_;
        break;
    }
    case 113:
        smoke_require(last_project_outcome_ == "opened" && !orbit_ && satellites_.empty() &&
                          stations_.size() == 1 && selected_station_ == 0 &&
                          snapshot_.markers.size() == 1,
                      "Station-only scenario incorrectly requires a satellite");
        begin_insert_station(InsertObjectMethod::defaults);
        ++orbit_smoke_stage_;
        break;
    case 114:
        smoke_require(last_project_outcome_ == "opened" && stations_.size() == 2 &&
                          selected_station_ == 1 &&
                          stations_[1].config.location == core::GroundStationLocation{},
                      "Default station insertion failed in a station-only scenario");
        begin_insert_satellite(InsertObjectMethod::defaults);
        ++orbit_smoke_stage_;
        break;
    case 115:
        smoke_require(last_orbit_outcome_ == "applied" && satellites_.size() == 1 &&
                          stations_.size() == 2 && snapshot_.markers.size() == 3 &&
                          selected_ == 0 &&
                          snapshot_.markers[1].object_id == stations_[0].config.object_id &&
                          snapshot_.markers[2].object_id == stations_[1].config.object_id,
                      "Inserting a satellite changed ground station marker identities");
        smoke_confirm_delete(true);
        ++orbit_smoke_stage_;
        break;
    case 116:
        smoke_require(last_project_outcome_ == "opened" && satellites_.empty() &&
                          stations_.size() == 2 && selected_station_ == 0,
                      "Deleting the last satellite failed to retain station objects");
        smoke_confirm_delete(true);
        ++orbit_smoke_stage_;
        break;
    case 117:
        smoke_require(last_project_outcome_ == "opened" && stations_.size() == 1 &&
                          selected_station_ == 0,
                      "Station-only deletion lost the surviving object");
        smoke_confirm_delete(true);
        ++orbit_smoke_stage_;
        break;
    case 118:
        smoke_require(last_project_outcome_ == "opened" && scenario_->objects().empty() &&
                          !selected_marker_index() && snapshot_.markers.empty() &&
                          markers_->size() == 0 && !delete_action_->isEnabled(),
                      "Deleting the last station left a ghost marker or enabled object action");
        smoke_record("ground-station-only-defaults-mixed-insertion-and-delete-last");
        start_project(application::load_project(std::filesystem::path(
                          (smoke_dir_ + "/scenario-edited.orbelle").toStdWString())),
                      smoke_dir_ + "/scenario-edited.orbelle");
        ++orbit_smoke_stage_;
        break;
    case 119:
        smoke_require(last_project_outcome_ == "opened" && stations_.empty() &&
                          !satellites_.empty(),
                      "Opening a satellite-only project retained old station objects");
        fit_views();
        smoke_final_frames_ = 0;
        ++orbit_smoke_stage_;
        break;
    case 120:
        if (++smoke_final_frames_ >= 4)
            ++orbit_smoke_stage_;
        break;
    default:
        throw std::logic_error("Stage does not belong to ground_stations");
    }
}
