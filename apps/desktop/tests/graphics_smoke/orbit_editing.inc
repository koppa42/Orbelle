void Workbench::smoke_orbit_editing() {
    auto input = orbit_ ? orbit_->input : OrbitInput{};
    switch (orbit_smoke_stage_) {
    case 0:
        // Start below the south pole to cover the former north-facing camera assumption.
        input.initial.position_m = {0, 0, -7e6};
        input.initial.velocity_mps = {6200, 5300, 0};
        editor_->set_input(input);
        editor_->set_state_mode(0);
        editor_->set_state_mode(1);
        smoke_require(editor_->findChild<QComboBox *>("stateRepresentation")->currentIndex() == 1,
                      "Classical element state conversion failed in the editor");
        editor_->submit();
        ++orbit_smoke_stage_;
        break;
    case 1:
        smoke_require(last_orbit_outcome_ == "applied" && selected_ == 0 &&
                          orbit_->input.object_id == "Satellite1" &&
                          orbit_->max_radius_m - orbit_->min_radius_m > 1e6,
                      "Edited inclined eccentric orbit was not published");
        smoke_record("apply-edited-initial-state");
        smoke_require(std::abs(orbit_->input.initial.position_m[0]) < 0.001 &&
                          std::abs(orbit_->input.initial.position_m[1]) < 0.001 &&
                          std::abs(orbit_->input.initial.position_m[2] + 7e6) < 0.001,
                      "Changing state representation changed the initial position beyond 1 mm");
        smoke_record("apply-keplerian-orbit");
        smoke_old_track_ = orbit_group_->children.front();
        slider_->setValue(10000);
        orbit_capture_ = true;
        ++orbit_smoke_stage_;
        break;
    case 2:
        smoke_require(snapshot_.epoch == orbit_->input.last, "Analysis end seek failed");
        smoke_record("seek-analysis-end");
        save_interaction_images("orbit-end");
        slider_->setValue(0);
        ++orbit_smoke_stage_;
        break;
    case 3:
        smoke_require(snapshot_.epoch == orbit_->input.first, "Analysis start seek failed");
        smoke_record("seek-analysis-start");
        save_interaction_images("orbit-start");
        orbit_capture_ = false;
        try {
            activate_scenario(
                application::Scenario("Invalid", core::Epoch{0}, core::Epoch{100'000'000'000'000}));
            throw std::runtime_error("Out-of-coverage scenario was accepted");
        } catch (const std::out_of_range &) {
            smoke_require(scenario_->name() == "Scenario1" &&
                              orbit_group_->children.front() == smoke_old_track_,
                          "Invalid scenario replaced the live scene");
        }
        smoke_record("reject-scenario-eop-coverage");
        input.initial.velocity_mps = {0, 0, 0};
        editor_->set_input(input);
        editor_->submit();
        ++orbit_smoke_stage_;
        break;
    case 4: {
        smoke_require(last_orbit_outcome_ == "rejected" &&
                          orbit_->track.samples().provenance().input_revision == 2 &&
                          orbit_group_->children.front() == smoke_old_track_,
                      "Invalid edit replaced the live scene");
        smoke_record("reject-invalid-initial-state");
        const auto error_message = editor_->findChild<QLabel *>("orbitResult")->text();
        smoke_require(!error_message.isEmpty() && !error_message.contains(QStringLiteral("正在")),
                      "Rejected orbit retained a pending status");
        set_selection(std::nullopt);
        smoke_require(editor_->findChild<QLabel *>("orbitResult")->text() == error_message,
                      "Changing selection overwrote the property panel's failed update result");
        set_selection(0);
        smoke_record("selection-preserves-property-error");
        input.step_seconds = 1;
        editor_->set_input(input);
        editor_->submit();
        cancellation_->store(true);
        orbit_smoke_stage_ = 6;
        break;
    }
    case 6:
        smoke_require(last_orbit_outcome_ == "cancelled" &&
                          orbit_->track.samples().provenance().input_revision == 2 &&
                          orbit_group_->children.front() == smoke_old_track_,
                      "Cancelled computation replaced the live scene");
        smoke_record("cancel-candidate");
        smoke_require(editor_->findChild<QLabel *>("orbitResult")->text() ==
                          QStringLiteral("已取消更新，原场景未改变。"),
                      "Cancelled orbit retained a pending status");
        smoke_old_track_ = {};
        for (auto &slot : slots_) {
            slot->window->hide();
            slot->window->destroy();
        }
        smoke_require(viewer_->windows().empty(), "Views remained after destroying both windows");
        input.initial.velocity_mps[0] += 1;
        editor_->set_input(input);
        editor_->submit();
        ++orbit_smoke_stage_;
        break;
    case 7:
        smoke_require(last_orbit_outcome_ == "applied" && viewer_->windows().empty() &&
                          orbit_->track.samples().provenance().input_revision == 3,
                      "Orbit could not be applied with all native windows closed");
        smoke_record("apply-without-windows");
        for (auto &slot : slots_) {
            slot->window->initializeWindow();
            slot->window->show();
        }
        fit_pending_ = true;
        ++orbit_smoke_stage_;
        break;
    case 8:
        if (slots_[0]->pick_candidates.empty() || slots_[1]->pick_candidates.empty())
            return;
        inject_click(*slots_[0], {slots_[0]->pick_candidates.front().x_pixels,
                                  slots_[0]->pick_candidates.front().y_pixels});
        orbit_capture_ = true;
        ++orbit_smoke_stage_;
        break;
    case 9:
        smoke_require(selected_ == std::optional<std::size_t>{0} && selection_->size() == 1,
                      "First view could not select the edited satellite");
        smoke_record("select-edited-orbit-view0");
        save_interaction_images("orbit-selected-view0");
        inject_click(*slots_[0], {0, 0});
        ++orbit_smoke_stage_;
        break;
    case 10:
        smoke_require(!selected_ && selection_->size() == 0,
                      "Edited orbit selection did not clear");
        smoke_require(!slots_[1]->pick_candidates.empty(), "Edited orbit disappeared from view 2");
        smoke_record("clear-edited-selection");
        inject_click(*slots_[1], {slots_[1]->pick_candidates.front().x_pixels,
                                  slots_[1]->pick_candidates.front().y_pixels});
        ++orbit_smoke_stage_;
        break;
    case 11:
        smoke_require(selected_ == std::optional<std::size_t>{0} && selection_->size() == 1,
                      "Second view could not select the edited satellite");
        smoke_record("select-edited-orbit-view1");
        save_interaction_images("orbit-selected-view1");
        orbit_capture_ = false;
        ++orbit_smoke_stage_;
        break;
    case 12:
        editor_->configure_appearance(1, {}, 1000);
        ++orbit_smoke_stage_;
        break;
    case 13:
        smoke_require(last_model_outcome_ == "applied" && model_kind_ == 1 && markers_->size() == 0,
                      "Built-in satellite model was not applied");
        smoke_record("built-in-satellite-model");
        focus_satellite();
        orbit_capture_ = true;
        ++orbit_smoke_stage_;
        break;
    case 14:
        save_interaction_images("satellite-model");
        editor_->configure_appearance(
            2, QCoreApplication::applicationDirPath() + "/resources/models/satellite_demo.gltf",
            1000);
        ++orbit_smoke_stage_;
        break;
    case 15:
        smoke_require(last_model_outcome_ == "applied" && model_kind_ == 2,
                      "Local glTF satellite model was not applied");
        smoke_record("local-gltf-satellite-model");
        ++orbit_smoke_stage_;
        break;
    case 16:
        save_interaction_images("satellite-gltf");
        editor_->configure_appearance(2, smoke_dir_ + "/absent-model.glb", 1000);
        ++orbit_smoke_stage_;
        break;
    case 17:
        smoke_require(last_model_outcome_ == "rejected" && model_kind_ == 2,
                      "Missing model replaced the existing satellite appearance");
        smoke_record("reject-missing-model");
        editor_->configure_appearance(0, {}, 1000);
        ++orbit_smoke_stage_;
        break;
    case 18:
        smoke_require(last_model_outcome_ == "applied" && model_kind_ == 0 && markers_->size() == 1,
                      "Point fallback was not restored");
        smoke_record("restore-point-marker");
        fit_views();
        orbit_capture_ = false;
        ++orbit_smoke_stage_;
        break;
    case 19:
        smoke_insert_dialog(InsertObjectMethod::define_properties);
        smoke_require(editor_->findChild<QLineEdit *>("objectName")->text() == "Satellite2",
                      "Insertion chooser did not generate a default satellite name");
        input.object_id = "Satellite1";
        editor_->set_input(input);
        editor_->submit();
        ++orbit_smoke_stage_;
        break;
    case 20:
        smoke_require(last_orbit_outcome_ == "rejected" && satellites_.size() == 1,
                      "Duplicate insertion changed the scene");
        smoke_record("reject-duplicate-object-id");
        input.object_id = "Satellite2";
        input.initial.position_m = {1e6, 0, -7e6};
        editor_->set_input(input);
        editor_->submit();
        ++orbit_smoke_stage_;
        break;
    case 21:
        smoke_require(
            last_orbit_outcome_ == "applied" && satellites_.size() == 2 &&
                scenario_->objects().size() == 2 && selected_ == 1 &&
                orbit_->session.provenance().input_revision == 3 && markers_->size() == 2,
            "Second insertion changed the first satellite or failed to select the new object");
        smoke_record("insert-second-satellite");
        input = satellites_[1].orbit->input;
        input.initial.velocity_mps[0] += 10;
        editor_->set_input(input);
        editor_->submit();
        ++orbit_smoke_stage_;
        break;
    case 22:
        smoke_require(last_orbit_outcome_ == "applied" &&
                          satellites_[1].orbit->session.provenance().input_revision == 2 &&
                          orbit_->session.provenance().input_revision == 3 && selected_ == 1,
                      "Editing satellite 2 affected satellite 1");
        smoke_record("independent-object-properties");
        editor_->configure_appearance(1, {}, 500);
        ++orbit_smoke_stage_;
        break;
    case 23:
        smoke_require(last_model_outcome_ == "applied" && satellites_[1].model_kind == 1 &&
                          satellites_[0].model_kind == 0 && markers_->size() == 1,
                      "Appearance was not applied only to the chosen satellite");
        smoke_record("independent-object-appearance");
        editor_->configure_appearance(0, {}, 500);
        ++orbit_smoke_stage_;
        break;
    case 24:
        smoke_require(last_model_outcome_ == "applied" && markers_->size() == 2,
                      "Second point marker was not restored");
        open_properties();
        input = satellites_[1].orbit->input;
        input.initial.position_m[0] += 1e6;
        editor_->set_input(input);
        properties_dialog_->close();
        smoke_require(satellites_[1].orbit->input.initial.position_m[0] !=
                          input.initial.position_m[0],
                      "Closing properties applied a draft");
        smoke_record("close-properties-without-applying");
        fit_views();
        set_selection(0);
        seek_epoch(scenario_->first());
        orbit_capture_ = true;
        ++orbit_smoke_stage_;
        break;
    case 25:
        save_workbench_image("scenario-two-satellites.png");
        orbit_capture_ = false;
        ++orbit_smoke_stage_;
        break;
    default:
        throw std::logic_error("Stage does not belong to orbit_editing");
    }
}
