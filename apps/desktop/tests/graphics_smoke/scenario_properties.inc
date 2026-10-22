void Workbench::smoke_scenario_properties() {
    auto input = orbit_ ? orbit_->input : OrbitInput{};
    switch (orbit_smoke_stage_) {
    case 95: {
        smoke_require(last_project_outcome_ == "opened" && satellites_.size() >= 2,
                      "Scenario editing needs the restored multi-object fixture");
        set_selection(satellites_.size() - 1);
        smoke_cache_ = satellites_[0].orbit;
        const auto revision = scenario_->revision();
        open_scenario_properties();
        auto *name = scenario_properties_->findChild<QLineEdit *>("scenarioSettingsName");
        auto *first = smoke_scenario_epoch_control("scenarioSettingsStart");
        auto *last = smoke_scenario_epoch_control("scenarioSettingsEnd");
        auto *apply = scenario_properties_->findChild<QPushButton *>("applyScenarioSettings");
        auto *reset = scenario_properties_->findChild<QPushButton *>("resetScenarioSettings");
        auto *result = scenario_properties_->findChild<QLabel *>("scenarioSettingsResult");
        smoke_require(name && first && last && apply && reset && !name->isReadOnly(),
                      "Missing editable scenario settings");
        apply->click();
        smoke_require(!busy() && !project_dirty_ && scenario_->revision() == revision,
                      "Unchanged settings created a revision or dirty project");
        name->setText("  ");
        apply->click();
        smoke_require(!busy() && !project_dirty_ && result->isVisible(),
                      "Invalid name did not preserve the scene");
        reset->click();
        first->set_epoch(scenario_->last());
        last->set_epoch(scenario_->first());
        apply->click();
        smoke_require(!busy() && !project_dirty_ && scenario_->revision() == revision,
                      "Reversed interval changed the scenario");
        reset->click();
        smoke_require(first->epoch() == scenario_->first() && last->epoch() == scenario_->last(),
                      "Reset did not restore applied bounds");
        smoke_record("scenario-settings-invalid-noop-and-reset-preserve-state");
        name->setText(QStringLiteral("调整后的分析场景"));
        workspace_->select_view(0);
        open_scenario_properties();
        smoke_require(scenario_properties_->findChild<QLineEdit *>("scenarioSettingsName") ==
                              name &&
                          name->text() == QStringLiteral("调整后的分析场景"),
                      "Returning to scenario properties discarded an unapplied draft");
        smoke_record("scenario-settings-draft-survives-view-switch");
        set_playing(true);
        apply->click();
        smoke_require(
            !busy() && project_dirty_ && playing_ && satellites_[0].orbit == smoke_cache_ &&
                scenario_->revision() == revision + 1 && scenario_item_->text(0) == name->text() &&
                workspace_label_->text().startsWith(name->text()),
            "Scenario rename rebuilt a cache or lost playback/title state");
        set_playing(false);
        smoke_record("scenario-rename-preserves-playback-caches-and-object-revisions");
        save_project_to(smoke_dir_ + "/scenario-edit-base.orbelle");
        smoke_settings_project_ = project_snapshot(project_path_);
        const auto span =
            scenario_->last().tai_nanoseconds() - scenario_->first().tai_nanoseconds();
        smoke_view_epoch_ = core::Epoch{scenario_->first().tai_nanoseconds() + span / 4 + 123};
        first->set_epoch(smoke_view_epoch_);
        last->set_epoch(core::Epoch{scenario_->last().tai_nanoseconds() - span / 4});
        // The live time is before the candidate range; publication must clamp to its start.
        seek_epoch(scenario_->first());
        snapshot_ = sample_frame(scenario_->first());
        update_satellite();
        apply->click();
        smoke_require(busy() && !apply->isEnabled() && !reset->isEnabled(),
                      "Scenario edits did not disable concurrent submissions");
        ++orbit_smoke_stage_;
        break;
    }
    case 96: {
        smoke_require(last_project_outcome_ == "opened" && project_dirty_ &&
                          scenario_->first() == smoke_view_epoch_ &&
                          snapshot_.epoch == smoke_view_epoch_,
                      "Changed scenario bounds lost the clamped physical time");
        smoke_require(selected_ == satellites_.size() - 1,
                      "Changed scenario bounds lost the selected satellite");
        smoke_require(QFileInfo(project_path_) ==
                          QFileInfo(smoke_dir_ + "/scenario-edit-base.orbelle"),
                      "Changed scenario bounds lost the project file identity");
        const auto &before = *smoke_settings_project_;
        for (std::size_t i = 0; i < satellites_.size(); ++i) {
            const auto &satellite = satellites_[i];
            const auto &settings_input = satellite.orbit->input;
            smoke_require(
                settings_input.first == scenario_->first() &&
                    settings_input.last == scenario_->last() &&
                    settings_input.initial.epoch == before.satellites[i].orbit.initial.epoch &&
                    settings_input.initial.position_m ==
                        before.satellites[i].orbit.initial.position_m &&
                    settings_input.initial.velocity_mps ==
                        before.satellites[i].orbit.initial.velocity_mps &&
                    satellite.display == before.satellites[i].display &&
                    satellite.name().toStdString() == before.satellites[i].name(),
                "Scenario resizing changed a satellite's physical input or display settings");
            smoke_require(scenario_->objects()[i].provenance().input_revision ==
                                  satellite.orbit->session.provenance().input_revision &&
                              cache_covers(*satellite.orbit, snapshot_.epoch, snapshot_.epoch),
                          "New bounds did not have consistent sessions and display caches");
        }
        smoke_require(satellites_[0].orbit->session.provenance().input_revision ==
                              smoke_cache_->session.provenance().input_revision &&
                          timeline_->current_time() ==
                              snapshot_.epoch.tai_nanoseconds() / 1'000'000,
                      "Scenario resizing changed numerical revisions or left a stale timeline");
        smoke_record("scenario-resize-rebuilds-all-displays-preserves-inputs-and-clamps-start");
        open_scenario_properties();
        QCoreApplication::sendPostedEvents(nullptr, QEvent::LayoutRequest);
        smoke_require(scenario_properties_->grab().save(smoke_dir_ + "/scenario-settings.png"),
                      "Cannot capture scenario settings");
        scenario_properties_->close();
        save_project_to(smoke_dir_ + "/scenario-edited.orbelle");
        smoke_settings_project_ = project_snapshot(project_path_);
        start_project(*smoke_settings_project_, project_path_);
        ++orbit_smoke_stage_;
        break;
    }
    case 97: {
        smoke_require(last_project_outcome_ == "opened" && !project_dirty_ &&
                          application::encode_project(project_snapshot(project_path_)) ==
                              application::encode_project(*smoke_settings_project_),
                      "Scenario settings did not survive reopening");
        smoke_record("scenario-edited-project-roundtrip");
        smoke_cache_ = satellites_[0].orbit;
        open_scenario_properties();
        auto *last = smoke_scenario_epoch_control("scenarioSettingsEnd");
        auto *apply = scenario_properties_->findChild<QPushButton *>("applyScenarioSettings");
        last->set_epoch(
            core::Epoch{eop_->data_info().last_epoch.tai_nanoseconds() + 5 * 86'400'000'000'000LL});
        smoke_answer_data(false, "dataCoverageDialog");
        apply->click();
        smoke_require(!busy() && !project_dirty_ && satellites_[0].orbit == smoke_cache_,
                      "Unavailable data during scenario editing changed the live scene");
        for (auto index : eop_catalog_->current_indices())
            if (eop_catalog_->providers()[index]->data_info().interpolation ==
                eop_->data_info().interpolation)
                eop_catalog_->set_extrapolation_days(index, 1);
        last->set_epoch(
            core::Epoch{eop_->data_info().last_epoch.tai_nanoseconds() + 600'000'000'000});
        smoke_answer_data(false);
        apply->click();
        smoke_require(!busy() && !project_dirty_ && satellites_[0].orbit == smoke_cache_ &&
                          scenario_->data_lock() == smoke_settings_project_->data_lock(),
                      "Declining replacement data during scenario editing changed the live scene");
        smoke_record("scenario-edit-coverage-and-data-decline-preserve-original");
        smoke_answer_data(true);
        apply->click();
        smoke_require(busy(), "Approved scenario edit was not submitted");
        ++orbit_smoke_stage_;
        break;
    }
    case 98: {
        smoke_require(scenario_properties_->findChild<QLabel *>("scenarioSettingsResult")->text() ==
                          QStringLiteral("场景设置已更新。"),
                      "Successful scenario update retained the pending hint");
        smoke_record("scenario-update-publishes-completion-status");
        smoke_require(last_project_outcome_ == "opened" && project_dirty_ &&
                          eop_->data_info().extrapolation_days == 1 &&
                          scenario_->data_lock() != smoke_settings_project_->data_lock(),
                      "Scenario resizing did not apply the approved data policy");
        for (const auto &satellite : satellites_)
            smoke_require(satellite.orbit->input.last == scenario_->last() &&
                              satellite.orbit->eop_source_id.find("limit-days=1") !=
                                  std::string::npos,
                          "Scenario editing mixed calculation policies");
        smoke_record("scenario-edit-data-consent-publishes-one-consistent-calculation");
        save_project_to(smoke_dir_ + "/scenario-edited.orbelle");
        smoke_settings_project_ = project_snapshot(project_path_);
        smoke_cache_ = satellites_[0].orbit;
        open_scenario_properties();
        auto *first = smoke_scenario_epoch_control("scenarioSettingsStart");
        first->set_epoch(core::Epoch{scenario_->first().tai_nanoseconds() + 1'000'000'000});
        scenario_properties_->findChild<QPushButton *>("applyScenarioSettings")->click();
        smoke_require(busy() && cancel_project_button_->isVisible(),
                      "Missing scenario update cancellation");
        cancel_project_button_->click();
        ++orbit_smoke_stage_;
        break;
    }
    case 99: {
        smoke_require(scenario_properties_->findChild<QLabel *>("scenarioSettingsResult")
                          ->text()
                          .startsWith(QStringLiteral("已取消更新")),
                      "Cancelled scenario update retained the pending hint");
        smoke_require(last_project_outcome_ == "cancelled" && !project_dirty_ &&
                          satellites_[0].orbit == smoke_cache_ &&
                          scenario_->first() == smoke_settings_project_->first &&
                          scenario_properties_->findChild<QPushButton *>("applyScenarioSettings")
                              ->isEnabled(),
                      "Cancelled scenario editing changed state or left the form locked");
        smoke_record("scenario-edit-cancellation-preserves-scene-and-restores-form");
        auto oversized = project_snapshot(project_path_);
        oversized.satellites[0].orbit.step_seconds = 1;
        oversized.satellites[0].display.extent = 1;
        start_project(
            std::move(oversized), project_path_, true, selected_, std::nullopt,
            ScenarioSettings{scenario_->name(), eop_->data_info().first_epoch, scenario_->last()});
        ++orbit_smoke_stage_;
        break;
    }
    case 100: {
        smoke_require(scenario_properties_->findChild<QLabel *>("scenarioSettingsResult")
                          ->text()
                          .startsWith(QStringLiteral("更新失败")),
                      "Failed scenario update retained the pending hint");
        smoke_require(last_project_outcome_ == "rejected" && !project_dirty_ &&
                          satellites_[0].orbit == smoke_cache_ &&
                          application::encode_project(project_snapshot(project_path_)) ==
                              application::encode_project(*smoke_settings_project_),
                      "A failed scene preparation published part of a scenario edit");
        smoke_record("scenario-edit-preparation-failure-retains-all-applied-state");
        scenario_properties_->findChild<QPushButton *>("resetScenarioSettings")->click();
        seek_epoch(scenario_->last());
        snapshot_ = sample_frame(scenario_->last());
        update_satellite();
        smoke_view_epoch_ = core::Epoch{scenario_->last().tai_nanoseconds() - 123'456'789};
        smoke_scenario_epoch_control("scenarioSettingsEnd")->set_epoch(smoke_view_epoch_);
        scenario_properties_->findChild<QPushButton *>("applyScenarioSettings")->click();
        ++orbit_smoke_stage_;
        break;
    }
    case 101:
        smoke_require(last_project_outcome_ == "opened" && project_dirty_ &&
                          snapshot_.epoch == smoke_view_epoch_ &&
                          scenario_->last() == smoke_view_epoch_,
                      "Scenario shrinking failed to clamp current time to the exact final epoch");
        smoke_record("scenario-edit-clamps-end-with-nanosecond-precision");
        save_project_to(smoke_dir_ + "/scenario-edited.orbelle");
        // A longer interval can put the final marker behind Earth from the
        // retained camera. Frame its current position for the pixel checks.
        fit_views();
        smoke_final_frames_ = 0;
        ++orbit_smoke_stage_;
        break;
    case 102:
        if (++smoke_final_frames_ >= 4) {
            save_workbench_image("scenario-edited-workspace.png");
            ++orbit_smoke_stage_;
        }
        break;
    default:
        throw std::logic_error("Stage does not belong to scenario_properties");
    }
}
