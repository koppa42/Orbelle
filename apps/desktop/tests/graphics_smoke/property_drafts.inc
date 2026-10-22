void Workbench::smoke_property_drafts() {
    auto input = orbit_ ? orbit_->input : OrbitInput{};
    switch (orbit_smoke_stage_) {
    case 172: {
        set_selection(0);
        open_properties();
        editor_->set_state_mode(0);
        auto *velocity = editor_->findChild<QuantityEditor *>("velocityY");
        auto *text = velocity->findChild<QLineEdit *>("quantityValue");
        text->setText("unfinished");
        smoke_cache_ = satellites_[0].orbit;
        properties_dialog_->close();
        open_properties();
        smoke_require(text->text() == "unfinished" && editor_->has_unapplied_changes(),
                      "Reopening a property page discarded incomplete input");
        editor_->submit();
        smoke_require(!busy() && satellites_[0].orbit == smoke_cache_ &&
                          editor_->has_unapplied_changes(),
                      "Invalid Apply published or discarded the draft");
        editor_->findChild<QPushButton *>("resetOrbit")->click();
        smoke_require(!editor_->has_unapplied_changes(), "Reset did not restore the applied orbit");
        editor_->set_state_mode(0);
        velocity->set_value(velocity->value(Unit::metres_per_second) + 0.25,
                            Unit::metres_per_second);
        save_project_to(smoke_dir_ + "/property-drafts.orbelle");
        const auto saved = application::load_project(
            std::filesystem::path((smoke_dir_ + "/property-drafts.orbelle").toStdWString()));
        smoke_require(same_orbit_input(saved.satellites[0].orbit, smoke_cache_->input) &&
                          !project_dirty_ && editor_->has_unapplied_changes(),
                      "Save incorrectly published a property draft");
        properties_dialog_->close();
        smoke_answer_property("leaveScenarioConfirmation", QMessageBox::Cancel);
        smoke_require(!confirm_replace_scenario() && workspace_->contains(properties_dialog_) &&
                          editor_->has_unapplied_changes(),
                      "Clean project exit failed to protect its unapplied draft");
        smoke_record("property-reopen-validation-reset-save-and-exit-preserve-draft");
        set_selection(satellites_.size());
        open_properties();
        station_editor_->findChild<QuantityEditor *>("stationHeight")->set_value(400, Unit::metre);
        station_editor_->close();
        open_properties();
        smoke_require(station_editor_->station().location.height_m == 400 &&
                          station_editor_->has_unapplied_changes(),
                      "Reopening a ground station discarded its draft");
        station_editor_->findChild<QPushButton *>("applyGroundStation")->click();
        ++orbit_smoke_stage_;
        break;
    }
    case 173: {
        smoke_require(
            last_project_outcome_ == "opened" && stations_[0].config.location.height_m == 400 &&
                workspace_->contains(station_editor_) &&
                !station_editor_->has_unapplied_changes() && editor_->has_unapplied_changes() &&
                same_orbit_input(satellites_[0].orbit->input, smoke_cache_->input),
            "Station Apply closed its page or discarded/applied another object's draft");
        smoke_record("station-apply-retains-page-and-independent-satellite-draft");
        auto project = project_snapshot(project_path_);
        auto duplicate = project.satellites[0];
        duplicate.orbit.object_id = "draft-audit-second";
        project.satellites.push_back(std::move(duplicate));
        start_project(std::move(project), project_path_, true, 0);
        ++orbit_smoke_stage_;
        break;
    }
    case 174: {
        smoke_require(satellites_.size() == 2 && editor_->has_unapplied_changes(),
                      "Inserting another object discarded a property draft");
        set_selection(1);
        smoke_answer_property("discardPropertyDraft", QMessageBox::Cancel);
        open_properties();
        smoke_require(editor_->bound_object_id() == satellites_[0].orbit->input.object_id &&
                          editor_->has_unapplied_changes(),
                      "Cancelling a property switch changed the editor binding");
        smoke_answer_property("discardPropertyDraft", QMessageBox::Discard);
        open_properties();
        smoke_require(editor_->bound_object_id() == satellites_[1].orbit->input.object_id &&
                          !editor_->has_unapplied_changes(),
                      "Explicitly discarding a draft did not switch the editor");
        smoke_record("object-switch-cancel-preserves-draft-and-discard-rebinds");
        smoke_cache_ = satellites_[1].orbit;
        editor_->set_state_mode(0);
        auto *velocity = editor_->findChild<QuantityEditor *>("velocityY");
        velocity->set_value(velocity->value(Unit::metres_per_second) + 1, Unit::metres_per_second);
        editor_->submit();
        smoke_require(candidate_.valid(),
                      "Draft cancellation did not start the expected orbit task");
        editor_->cancel();
        ++orbit_smoke_stage_;
        break;
    }
    case 175: {
        smoke_require(last_orbit_outcome_ == "cancelled" && satellites_[1].orbit == smoke_cache_ &&
                          editor_->has_unapplied_changes() &&
                          workspace_->contains(properties_dialog_),
                      "Cancelling Apply did not preserve the editable draft and applied state");
        editor_->findChild<QPushButton *>("resetOrbit")->click();
        smoke_record("cancelled-orbit-apply-retains-draft-and-page");
        begin_insert_station();
        station_editor_->findChild<QLineEdit *>("stationName")->setText("Pending station");
        station_editor_->close();
        begin_insert_station();
        smoke_require(station_editor_->station().name() == "Pending station",
                      "Reopening station creation discarded its draft");
        set_selection(satellites_.size());
        smoke_answer_property("discardPropertyDraft", QMessageBox::Discard);
        open_properties();
        smoke_require(!station_editor_->has_unapplied_changes(),
                      "Switching from station creation did not load applied values");
        set_selection(0);
        edit_sensor(true);
        sensor_editor_->findChild<QLineEdit *>("sensorName")->setText("Pending sensor");
        sensor_editor_->close();
        edit_sensor(true);
        smoke_require(sensor_editor_->sensor().name() == "Pending sensor",
                      "Reopening sensor creation discarded its draft");
        sensor_editor_->findChild<QPushButton *>("applySensor")->click();
        ++orbit_smoke_stage_;
        break;
    }
    case 176: {
        smoke_require(last_project_outcome_ == "opened" && sensors_.size() == 1 &&
                          workspace_->contains(sensor_editor_) && !sensor_editor_creating_ &&
                          !sensor_editor_->has_unapplied_changes(),
                      "Sensor Apply closed its page or retained insertion mode");
        sensor_editor_->findChild<QuantityEditor *>("sensorHalfAngle")->set_value(20, Unit::degree);
        sensor_editor_->close();
        edit_sensor(false);
        smoke_require(sensor_editor_->has_unapplied_changes() &&
                          std::abs(sensor_editor_->sensor().pattern.half_angle_rad -
                                   20 * std::numbers::pi / 180) < 1e-12,
                      "Reopening an existing sensor discarded its draft");
        sensor_editor_->findChild<QPushButton *>("resetSensor")->click();
        smoke_record("station-and-sensor-creation-reopen-and-successful-apply");
        open_scenario_properties();
        auto *name = scenario_properties_->findChild<QLineEdit *>("scenarioSettingsName");
        name->setText("Pending scenario title");
        scenario_properties_->close();
        open_scenario_properties();
        smoke_require(name->text() == "Pending scenario title" && scenario_has_draft(),
                      "Reopening the scene properties discarded its draft");
        // A different property update must preserve this draft and refresh read-only facts.
        set_selection(satellites_.size());
        open_properties();
        station_editor_->findChild<QuantityEditor *>("stationHeight")->set_value(450, Unit::metre);
        station_editor_->findChild<QPushButton *>("applyGroundStation")->click();
        ++orbit_smoke_stage_;
        break;
    }
    case 177: {
        smoke_require(last_project_outcome_ == "opened" && scenario_has_draft() &&
                          scenario_->name() != "Pending scenario title",
                      "Unrelated property update discarded or applied a scene draft");
        smoke_answer_property("leaveScenarioConfirmation", QMessageBox::Save, true);
        smoke_require(confirm_replace_scenario() && !project_dirty_ && scenario_has_draft(),
                      "Save-and-leave did not preserve the pending draft until actual replacement");
        const auto saved =
            application::load_project(std::filesystem::path(project_path_.toStdWString()));
        smoke_require(saved.name == scenario_->name(),
                      "Exit save accidentally applied a scene draft");
        smoke_answer_property("leaveScenarioConfirmation", QMessageBox::Discard);
        smoke_require(confirm_replace_scenario() && scenario_has_draft(),
                      "Discard consent changed state before the replacement was prepared");
        scenario_properties_->findChild<QPushButton *>("resetScenarioSettings")->click();
        smoke_require(unapplied_properties().empty(), "Reset left phantom property drafts");
        smoke_record("scene-draft-survives-other-updates-and-exit-saves-applied-state-only");
        for (auto *page :
             {properties_dialog_, scenario_properties_, static_cast<QDialog *>(station_editor_),
              static_cast<QDialog *>(sensor_editor_)})
            page->close();
        // The last edit selected a ground station. Fit to a satellite for the
        // final marker-pixel assertion instead of leaving it behind the globe.
        set_selection(0);
        seek_epoch(scenario_->first());
        fit_views();
        fit_pending_ = true;
        smoke_final_frames_ = 0;
        ++orbit_smoke_stage_;
        break;
    }
    default:
        throw std::logic_error("Stage does not belong to property_drafts");
    }
}
