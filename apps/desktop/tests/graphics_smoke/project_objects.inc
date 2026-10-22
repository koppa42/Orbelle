void Workbench::smoke_project_objects() {
    auto input = orbit_ ? orbit_->input : OrbitInput{};
    switch (orbit_smoke_stage_) {
    case 68: {
        if (frame_count_ < smoke_wait_frame_ || !view_captures_ready())
            return;
        smoke_cache_ = orbit_;
        set_selection(0);
        slots_[1]->dock->hide();
        open_properties();
        auto *frame = editor_->findChild<QComboBox *>("initialReferenceFrame");
        smoke_require(frame && !editor_->findChild<QuantityEditor *>("gravitationalParameter"),
                      "Satellite properties must select a frame and inherit the body's mu");
        frame->setCurrentIndex(1);
        const auto &initial = orbit_->input.initial;
        const auto expected =
            frames_->gcrf_to_itrf_state(initial.epoch, eop_->at(initial.epoch)).apply(initial);
        smoke_require(frame->currentIndex() == 1 &&
                          std::abs(editor_->findChild<QuantityEditor *>("velocityY")
                                       ->value(Unit::metres_per_second) -
                                   expected.velocity_mps[1]) < 1e-6,
                      "The initial ITRF state did not include the velocity rotation term");
        QCoreApplication::sendPostedEvents(nullptr, QEvent::LayoutRequest);
        save_workbench_image("initial-state-itrf.png");
        frame->setCurrentIndex(0);
        editor_->set_state_mode(1);
        editor_->findChild<QComboBox *>("orbitPropagator")->setCurrentIndex(1);
        smoke_require(editor_->findChild<QuantityEditor *>("meanAnomaly"),
                      "J2 input must identify the mean anomaly");
        QTimer::singleShot(0, editor_, [this] {
            auto *dialog = editor_->findChild<QDialog *>("propagatorDetailsDialog");
            smoke_require(
                dialog && dialog->findChildren<QCheckBox *>().empty() &&
                    dialog->findChildren<QuantityEditor *>().empty(),
                "Model information must not duplicate sampling or offer partial J2 terms");
            smoke_require(dialog->grab().save(smoke_dir_ + "/j2-model.png"),
                          "Cannot capture J2 model information");
            dialog->findChild<QDialogButtonBox *>()->button(QDialogButtonBox::Close)->click();
        });
        timer_.stop();
        editor_->findChild<QPushButton *>("propagatorDetails")->click();
        timer_.start();
        editor_->submit();
        smoke_record("initial-state-frame-conversion-and-j2-model-details");
        ++orbit_smoke_stage_;
        break;
    }
    case 69:
        smoke_require(last_orbit_outcome_ == "applied" &&
                          orbit_->input.propagator == OrbitPropagator::j2_secular &&
                          orbit_->max_inertial_check_error_m <= 1 &&
                          orbit_->session.provenance().algorithm.find("node=1;apsis=1;mean=1") !=
                              std::string::npos,
                      "J2 propagation did not publish every secular term");
        save_workbench_image("j2-properties.png");
        properties_dialog_->close();
        slots_[1]->dock->show();
        fit_views();
        smoke_wait_frame_ = frame_count_ + 3;
        ++orbit_smoke_stage_;
        break;
    case 70:
        if (frame_count_ < smoke_wait_frame_ || !view_captures_ready())
            return;
        save_workbench_image("j2-orbit.png");
        smoke_record("publish-j2-trajectory-with-validated-display-cache");
        editor_->set_input(smoke_cache_->input);
        editor_->submit();
        ++orbit_smoke_stage_;
        break;
    case 71:
        smoke_require(last_orbit_outcome_ == "applied" &&
                          orbit_->input.propagator == OrbitPropagator::two_body,
                      "Could not switch back from J2 to two-body propagation");
        smoke_record("switch-propagator-back-to-two-body");
        orbit_capture_ = false;
        begin_insert_satellite();
        input = satellites_.front().orbit->input;
        input.object_id = "ProjectJ2";
        input.propagator = OrbitPropagator::j2_secular;
        editor_->set_input(input);
        editor_->submit();
        ++orbit_smoke_stage_;
        break;
    case 72:
        smoke_require(last_orbit_outcome_ == "applied" && satellites_.size() == 2 &&
                          satellites_[1].orbit->input.propagator == OrbitPropagator::j2_secular,
                      "Project test did not create two different propagators");
        satellites_[1].display.extent = 2;
        satellites_[1].display.past_minutes = 12.5;
        satellites_[1].display.future_minutes = 6.25;
        satellites_[1].display.labels_2d = false;
        editor_->configure_appearance(1, {}, 250);
        ++orbit_smoke_stage_;
        break;
    case 73: {
        smoke_require(last_model_outcome_ == "applied" && project_dirty_,
                      "Project changes were not marked unsaved");
        set_playing(false);
        const core::Epoch epoch{scenario_->first().tai_nanoseconds() + 123'456'789'123};
        seek_epoch(epoch);
        snapshot_ = sample_frame(epoch);
        update_satellite();
        const auto path = smoke_dir_ + QStringLiteral("/项目.orbelle");
        const auto recorded = project_snapshot(path);
        analysis_record_ = application::AnalysisRecord{application::analysis_input_sha256(recorded),
                                                       recorded.data_lock().sha256()};
        standalone_time_data_ = eop_->data_info().time_data;
        save_project_to(path);
        smoke_require(!project_dirty_, "Successful save did not clear the modified marker");
        smoke_project_bytes_ = application::encode_project(project_snapshot(path));
        auto saved = application::load_project(std::filesystem::path(path.toStdWString()));
        smoke_require(saved.current == epoch, "Saved current time lost nanoseconds");
        start_project(std::move(saved), path);
        ++orbit_smoke_stage_;
        break;
    }
    case 74: {
        smoke_require(last_project_outcome_ == "opened" && !project_dirty_ &&
                          satellites_.size() == 2 && satellites_[1].model_kind == 1 &&
                          satellites_[1].model_diameter_km == 250 &&
                          !satellites_[1].display.labels_2d &&
                          satellites_[1].orbit->input.propagator == OrbitPropagator::j2_secular,
                      "Saved project did not restore objects and display settings");
        smoke_require(application::encode_project(project_snapshot(project_path_)) ==
                          smoke_project_bytes_,
                      "Desktop project roundtrip changed its persisted inputs");
        smoke_require(analysis_record_ && standalone_time_data_ &&
                          project_snapshot(project_path_).analysis_status() ==
                              application::AnalysisStatus::current,
                      "Desktop project roundtrip lost API analysis provenance");
        smoke_record("project-save-open-two-body-j2-display-nanoseconds");
        save_workbench_image("project-restored.png");
        smoke_cache_ = orbit_;
        const auto smoke_time = runtime::make_builtin_time_provider();
        application::Project uncovered{"Uncovered", smoke_time->from_utc({2027, 1, 1, 0, 0, 0, 0}),
                                       smoke_time->from_utc({2027, 1, 2, 0, 0, 0, 0})};
        smoke_answer_data(false, "dataCoverageDialog");
        smoke_require(!start_project(uncovered, project_path_) && orbit_ == smoke_cache_ &&
                          !busy() && !project_dirty_,
                      "Coverage failure replaced the current scene");
        smoke_record("project-coverage-gap-preserves-scene");
        auto wrong_data = project_snapshot(project_path_);
        wrong_data.earth_orientation->sha256[0] =
            wrong_data.earth_orientation->sha256[0] == 'a' ? 'b' : 'a';
        smoke_answer_data(false);
        smoke_require(!start_project(wrong_data, project_path_) && !busy() &&
                          orbit_ == smoke_cache_ && !project_dirty_,
                      "Declining changed data modified the scene");
        smoke_record("project-data-change-decline-preserves-scene");
        smoke_answer_data(true);
        start_project(std::move(wrong_data), project_path_);
        ++orbit_smoke_stage_;
        break;
    }
    case 75: {
        smoke_require(last_project_outcome_ == "opened" && satellites_.size() == 2 &&
                          project_dirty_ &&
                          scenario_->data_lock() == application::eop_data_lock(eop_->data_info()),
                      "Saved data hash prevented the project from using current data");
        smoke_record("project-data-change-approval-uses-current-data");
        smoke_cache_ = orbit_;
        auto missing_model = project_snapshot(project_path_);
        missing_model.satellites[1].model_kind = 2;
        missing_model.satellites[1].model_path = "missing-project-model.glb";
        start_project(std::move(missing_model), project_path_);
        ++orbit_smoke_stage_;
        break;
    }
    case 76: {
        smoke_require(last_project_outcome_ == "rejected" && orbit_ == smoke_cache_ &&
                          satellites_.size() == 2,
                      "A missing external model replaced the active project");
        smoke_record("project-missing-model-preserves-active-scene");
        mark_project_dirty();
        QTimer::singleShot(0, [] {
            if (auto *box = qobject_cast<QMessageBox *>(QApplication::activeModalWidget()))
                box->button(QMessageBox::Cancel)->click();
        });
        smoke_require(!confirm_replace_scenario() && project_dirty_,
                      "Cancel discarded unsaved changes");
        QTimer::singleShot(0, [] {
            if (auto *box = qobject_cast<QMessageBox *>(QApplication::activeModalWidget()))
                box->button(QMessageBox::Save)->click();
        });
        smoke_require(confirm_replace_scenario() && !project_dirty_, "Save-before-replace failed");
        smoke_record("project-unsaved-cancel-and-save");
        auto empty = application::Project{"Empty project", scenario_->first(), scenario_->last()};
        empty.earth_orientation = eop_->data_info();
        start_project(std::move(empty), smoke_dir_ + "/empty.orbelle");
        ++orbit_smoke_stage_;
        break;
    }
    case 77: {
        smoke_require(last_project_outcome_ == "opened" && satellites_.empty() &&
                          scenario_->objects().empty() && ground_map_->track_count() == 0,
                      "Empty project did not clear all objects");
        smoke_record("project-open-empty-scene");
        const auto args = app_.arguments();
        const auto fixture_index = args.indexOf("--project-smoke-input");
        const auto path = fixture_index >= 0 ? args.value(fixture_index + 1)
                                             : smoke_dir_ + QStringLiteral("/项目.orbelle");
        start_project(application::load_project(std::filesystem::path(path.toStdWString())), path);
        ++orbit_smoke_stage_;
        break;
    }
    case 78:
        smoke_require(last_project_outcome_ == "opened" && satellites_.size() == 2 &&
                          satellites_[0].orbit->input.propagator == OrbitPropagator::two_body &&
                          satellites_[1].orbit->input.propagator == OrbitPropagator::j2_secular,
                      "Project interoperability input failed to open");
        save_project_to(smoke_dir_ + "/desktop-roundtrip.orbelle");
        smoke_record("project-interoperability-load-and-save");
        smoke_cache_ = orbit_;
        start_project(project_snapshot(project_path_), project_path_);
        project_cancellation_->store(true);
        ++orbit_smoke_stage_;
        break;
    case 79:
        smoke_require(last_project_outcome_ == "cancelled" && orbit_ == smoke_cache_ &&
                          !project_dirty_,
                      "Cancelling project preparation changed the current scene");
        smoke_record("project-cancel-open-preserves-current-scene");
        ++orbit_smoke_stage_;
        break;
    case 80: {
        set_selection(0);
        smoke_cache_ = orbit_;
        smoke_data_lock_ = scenario_->data_lock();
        QTimer::singleShot(0, [] {
            if (auto *dialog = qobject_cast<QInputDialog *>(QApplication::activeModalWidget())) {
                dialog->setTextValue(QStringLiteral("观测卫星 A"));
                dialog->accept();
            }
        });
        rename_action_->trigger();
        smoke_require(satellites_[0].name() == QStringLiteral("观测卫星 A") &&
                          orbit_ == smoke_cache_ && project_dirty_ &&
                          scenario_->data_lock() == smoke_data_lock_,
                      "Rename changed the numerical session or failed to update the name");
        save_project_to(smoke_dir_ + "/objects.orbelle");
        const auto named =
            application::load_project(std::filesystem::path(project_path_.toStdWString()));
        smoke_require(named.satellites[0].display_name == "观测卫星 A" &&
                          named.satellites[0].orbit.object_id == smoke_cache_->input.object_id,
                      "Rename did not persist separately from the stable ID");
        smoke_record("object-rename-keeps-session-and-stable-id");
        open_properties();
        editor_->findChild<QLineEdit *>("objectName")->setText(QStringLiteral("属性页卫星"));
        editor_->submit();
        smoke_require(!busy() && orbit_ == smoke_cache_ && !editor_->has_unapplied_changes() &&
                          satellites_[0].name() == QStringLiteral("属性页卫星") &&
                          runtime::report_input_sha256(project_snapshot(project_path_)) ==
                              runtime::report_input_sha256(named),
                      "Name-only satellite Apply recalculated or changed report inputs");
        editor_->findChild<QLineEdit *>("objectName")->setText(QStringLiteral("观测卫星 A"));
        editor_->submit();
        smoke_record("satellite-name-apply-preserves-session-and-reports");
        smoke_view_epoch_ = snapshot_.epoch;
        duplicate_action_->trigger();
        ++orbit_smoke_stage_;
        break;
    }
    case 81: {
        smoke_require(
            last_project_outcome_ == "opened" && satellites_.size() == 3 && selected_ == 2 &&
                project_dirty_ && snapshot_.epoch == smoke_view_epoch_ &&
                satellites_[0].orbit->session.provenance().input_revision ==
                    smoke_cache_->session.provenance().input_revision &&
                satellites_[2].orbit->input.object_id != satellites_[0].orbit->input.object_id &&
                satellites_[2].display == satellites_[0].display &&
                satellites_[2].model_kind == satellites_[0].model_kind &&
                satellites_[2].orbit->input.initial.position_m ==
                    satellites_[0].orbit->input.initial.position_m,
            "Copy lost settings, changed the original, or reused its identity");
        smoke_record("object-copy-preserves-settings-and-current-time");
        open_properties();
        auto copied = satellites_[2].orbit->input;
        copied.initial.position_m[0] *= 1.001;
        editor_->set_input(copied);
        editor_->submit();
        ++orbit_smoke_stage_;
        break;
    }
    case 82:
        smoke_require(last_orbit_outcome_ == "applied" &&
                          satellites_[2].orbit->session.provenance().input_revision == 2 &&
                          satellites_[0].orbit->input.initial.position_m ==
                              smoke_cache_->input.initial.position_m,
                      "Editing a copy changed the original object");
        smoke_record("object-copy-is-independently-editable");
        properties_dialog_->close();
        smoke_confirm_delete(false);
        smoke_require(satellites_.size() == 3 && !busy(), "Cancel deleted an object");
        smoke_record("object-delete-cancel-preserves-scene");
        smoke_confirm_delete(true);
        ++orbit_smoke_stage_;
        break;
    case 83:
        smoke_require(last_project_outcome_ == "opened" && satellites_.size() == 2 &&
                          selected_ == 1 && ground_map_->track_count() == 2 &&
                          scenario_->objects().size() == 2,
                      "Deleting a copy did not synchronize the scene and selection");
        set_selection(0);
        smoke_confirm_delete(true);
        ++orbit_smoke_stage_;
        break;
    case 84:
        smoke_require(last_project_outcome_ == "opened" && satellites_.size() == 1 &&
                          selected_ == 0 &&
                          satellites_[0].orbit->input.propagator == OrbitPropagator::j2_secular,
                      "Deleting the first object left stale indices");
        smoke_record("object-delete-reindexes-surviving-objects");
        smoke_confirm_delete(true);
        ++orbit_smoke_stage_;
        break;
    case 85: {
        smoke_require(last_project_outcome_ == "opened" && satellites_.empty() && !selected_ &&
                          scenario_->objects().empty() && ground_map_->track_count() == 0 &&
                          orbit_group_->children.empty() && model_group_->children.empty() &&
                          !delete_action_->isEnabled() && snapshot_.markers.empty(),
                      "Deleting the last object left stale graphical resources");
        smoke_record("object-delete-last-produces-valid-empty-scene");
        const auto path = smoke_dir_ + "/objects.orbelle";
        start_project(application::load_project(std::filesystem::path(path.toStdWString())), path);
        ++orbit_smoke_stage_;
        break;
    }
    default:
        throw std::logic_error("Stage does not belong to project_objects");
    }
}
