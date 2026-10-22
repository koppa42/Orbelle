void Workbench::smoke_specialist_reports() {
    auto input = orbit_ ? orbit_->input : OrbitInput{};
    switch (orbit_smoke_stage_) {
    case 178: {
        application::Project workflow{
            "Covariance and mission audit", scenario_->first(),
            core::Epoch{scenario_->first().tai_nanoseconds() + 600'000'000'000LL}};
        application::ProjectSatellite satellite;
        satellite.orbit.object_id = "workflow-audit";
        satellite.orbit.first = workflow.first;
        satellite.orbit.last = workflow.last;
        satellite.orbit.initial.epoch = workflow.first;
        satellite.orbit.propagator = OrbitPropagator::hpop;
        workflow.satellites.push_back(satellite);
        start_project(std::move(workflow), smoke_dir_ + "/specialist-workflow.orbelle");
        ++orbit_smoke_stage_;
        break;
    }
    case 179:
        smoke_require(last_project_outcome_ == "opened", "Specialist workflow did not open");
        set_selection(0);
        open_properties();
        smoke_edit_covariance(10000, true);
        smoke_require(editor_->has_unapplied_changes() && !orbit_->input.covariance.enabled,
                      "Saving covariance settings applied them without Apply");
        editor_->submit();
        ++orbit_smoke_stage_;
        break;
    case 180:
        smoke_require(last_orbit_outcome_ == "applied" && orbit_->input.covariance.enabled &&
                          orbit_->input.covariance.state[0][0] == 10000 &&
                          !editor_->has_unapplied_changes(),
                      "Covariance Apply did not publish the accepted settings");
        show_reports();
        report_dialog_->select_object("workflow-audit", application::ReportKind::state_covariance);
        report_dialog_->findChild<QDoubleSpinBox *>("reportStep")->setValue(60);
        report_dialog_->findChild<QPushButton *>("generateReport")->click();
        ++orbit_smoke_stage_;
        break;
    case 181: {
        if (!report_dialog_->findChild<QPushButton *>("generateReport")->isEnabled())
            break;
        const auto *model = report_dialog_->findChild<QTableView *>("reportTable")->model();
        smoke_require(model->rowCount() == 11 &&
                          model->index(0, smoke_report_column(model, "covariance.x.x"))
                                  .data(Qt::UserRole)
                                  .toDouble() == 10000,
                      "Covariance report did not use the applied SI matrix");
        save_workbench_image("covariance-applied-report.png");
        open_properties();
        smoke_edit_covariance(90000, false);
        smoke_require(!editor_->has_unapplied_changes(),
                      "Cancelling covariance settings modified the draft");
        smoke_edit_covariance(40000, true);
        smoke_cache_ = orbit_;
        editor_->submit();
        smoke_require(candidate_.valid(), "Covariance edit did not start a task");
        editor_->cancel();
        ++orbit_smoke_stage_;
        break;
    }
    case 182:
        smoke_require(last_orbit_outcome_ == "cancelled" && orbit_ == smoke_cache_ &&
                          editor_->has_unapplied_changes() &&
                          report_dialog_->findChild<QPushButton *>("exportReportCsv")->isEnabled(),
                      "Cancelled covariance Apply lost the draft or invalidated the old report");
        smoke_record("covariance-settings-and-apply-cancel-preserve-published-state");
        editor_->submit();
        ++orbit_smoke_stage_;
        break;
    case 183: {
        smoke_require(last_orbit_outcome_ == "applied" &&
                          orbit_->input.covariance.state[0][0] == 40000,
                      "Retrying covariance Apply did not publish the retained draft");
        const auto *model = report_dialog_->findChild<QTableView *>("reportTable")->model();
        smoke_require(model->index(0, smoke_report_column(model, "covariance.x.x"))
                                  .data(Qt::UserRole)
                                  .toDouble() == 10000 &&
                          report_dialog_->findChild<QLabel *>("reportResultLabel")
                              ->text()
                              .contains(QStringLiteral("场景已变化")) &&
                          !report_dialog_->findChild<QAction *>("reportChooseColumns")->isEnabled(),
                      "Applied covariance failed to retain and mark the old report stale");
        show_reports();
        report_dialog_->findChild<QPushButton *>("generateReport")->click();
        ++orbit_smoke_stage_;
        break;
    }
    case 184: {
        if (!report_dialog_->findChild<QPushButton *>("generateReport")->isEnabled())
            break;
        const auto *model = report_dialog_->findChild<QTableView *>("reportTable")->model();
        smoke_require(model->rowCount() == 11 &&
                          model->index(0, smoke_report_column(model, "covariance.x.x"))
                                  .data(Qt::UserRole)
                                  .toDouble() == 40000,
                      "Regenerated covariance report reused stale settings");
        smoke_record("covariance-apply-invalidates-report-and-regenerate-uses-new-matrix");
        save_project_to(smoke_dir_ + "/covariance-workflow.orbelle");
        const auto restored =
            application::load_project(std::filesystem::path(project_path_.toStdWString()));
        smoke_require(restored.satellites[0].orbit.covariance == orbit_->input.covariance,
                      "Covariance persistence changed applied settings");
        start_project(restored, project_path_);
        ++orbit_smoke_stage_;
        break;
    }
    case 185: {
        smoke_require(last_project_outcome_ == "opened" && orbit_->input.covariance.enabled &&
                          orbit_->input.covariance.state[0][0] == 40000,
                      "Reopening the project lost covariance settings");
        smoke_record("covariance-save-reopen-preserves-applied-configuration");
        set_selection(0);
        open_properties();
        editor_->set_propagator(OrbitPropagator::mission);
        bool configured = false;
        QTimer::singleShot(0, this, [&] {
            auto *dialog = qobject_cast<QDialog *>(QApplication::activeModalWidget());
            smoke_require(dialog && dialog->objectName() == "missionEditorDialog",
                          "Mission editor did not open");
            dialog->findChild<QComboBox *>("missionCoastModel")->setCurrentIndex(0);
            dialog->findChild<QuantityEditor *>("missionCoastDuration")
                ->set_value(300, Unit::second);
            dialog->findChild<QPushButton *>("missionAddImpulse")->click();
            dialog->findChild<QLineEdit *>("missionSegmentName")->setText("Transfer burn");
            dialog->findChild<QComboBox *>("missionManeuverFrame")->setCurrentIndex(0);
            dialog->findChild<QuantityEditor *>("missionDeltaV1")
                ->set_value(10, Unit::metres_per_second);
            dialog->findChild<QPushButton *>("missionAddCoast")->click();
            dialog->findChild<QuantityEditor *>("missionCoastDuration")
                ->set_value(300, Unit::second);
            configured = true;
            dialog->findChild<QDialogButtonBox *>()->button(QDialogButtonBox::Ok)->click();
        });
        timer_.stop();
        editor_->open_propagator_details();
        timer_.start();
        smoke_require(configured && editor_->has_unapplied_changes() &&
                          orbit_->input.propagator == OrbitPropagator::hpop,
                      "Saving a mission plan applied it without Apply");
        editor_->submit();
        ++orbit_smoke_stage_;
        break;
    }
    case 186: {
        smoke_require(last_orbit_outcome_ == "applied" &&
                          orbit_->input.propagator == OrbitPropagator::mission &&
                          !orbit_->input.covariance.enabled,
                      "Mission Apply retained unsupported active covariance");
        const auto burns = orbit_->session.maneuvers(scenario_->first(), scenario_->last());
        smoke_require(burns.size() == 1 && burns[0].name == "Transfer burn" &&
                          burns[0].after.epoch.tai_nanoseconds() ==
                              scenario_->first().tai_nanoseconds() + 300'000'000'000LL &&
                          burns[0].delta_v_gcrf_mps == std::array<double, 3>{0, 10, 0},
                      "Applied mission did not preserve the configured burn");
        smoke_cache_ = orbit_;
        show_reports();
        report_dialog_->select_object("workflow-audit", application::ReportKind::cartesian_state);
        report_dialog_->findChild<QComboBox *>("reportFrame")->setCurrentIndex(0);
        report_dialog_->findChild<QDoubleSpinBox *>("reportStep")->setValue(60);
        report_dialog_->findChild<QPushButton *>("generateReport")->click();
        ++orbit_smoke_stage_;
        break;
    }
    case 187: {
        if (!report_dialog_->findChild<QPushButton *>("generateReport")->isEnabled())
            break;
        const auto *model = report_dialog_->findChild<QTableView *>("reportTable")->model();
        const auto burns = orbit_->session.maneuvers(scenario_->first(), scenario_->last());
        smoke_require(model->rowCount() == 11, "Mission report has the wrong sampling interval");
        for (int i = 0; i < 3; ++i) {
            const auto position = std::array{"state.x", "state.y", "state.z"}[i];
            const auto velocity = std::array{"state.vx", "state.vy", "state.vz"}[i];
            smoke_require(model->index(5, smoke_report_column(model, position))
                                      .data(Qt::UserRole)
                                      .toDouble() == burns[0].after.position_m[i] &&
                              model->index(5, smoke_report_column(model, velocity))
                                      .data(Qt::UserRole)
                                      .toDouble() == burns[0].after.velocity_mps[i],
                          "Mission report at a burn did not use the exact post-burn state");
        }
        save_workbench_image("mission-applied-report.png");
        smoke_record("mission-plan-apply-and-report-preserve-exact-burn-boundary");
        open_properties();
        QTimer::singleShot(0, this, [] {
            auto *dialog = qobject_cast<QDialog *>(QApplication::activeModalWidget());
            smoke_require(dialog && dialog->objectName() == "missionEditorDialog",
                          "Mission editor did not reopen");
            dialog->findChild<QListWidget *>("missionSegments")->setCurrentRow(1);
            dialog->findChild<QLineEdit *>("missionSegmentName")->setText("Renamed burn");
            dialog->findChild<QuantityEditor *>("missionDeltaV1")
                ->set_value(20, Unit::metres_per_second);
            dialog->findChild<QDialogButtonBox *>()->button(QDialogButtonBox::Ok)->click();
        });
        timer_.stop();
        editor_->open_propagator_details();
        timer_.start();
        editor_->submit();
        ++orbit_smoke_stage_;
        break;
    }
    case 188: {
        smoke_require(
            last_orbit_outcome_ == "applied" &&
                orbit_->session.maneuvers(scenario_->first(), scenario_->last())[0].name ==
                    "Renamed burn" &&
                smoke_cache_->session.maneuvers(scenario_->first(), scenario_->last())[0].name ==
                    "Transfer burn",
            "Editing a mission segment reused or mutated an old scientific session");
        smoke_require(report_dialog_->findChild<QLabel *>("reportResultLabel")
                          ->text()
                          .contains(QStringLiteral("场景已变化")),
                      "Mission Apply did not mark the old report stale");
        save_project_to(smoke_dir_ + "/mission-workflow.orbelle");
        const auto restored =
            application::load_project(std::filesystem::path(project_path_.toStdWString()));
        smoke_require(restored.satellites[0].orbit.mission == orbit_->input.mission,
                      "Mission persistence changed the sequence");
        start_project(restored, project_path_);
        ++orbit_smoke_stage_;
        break;
    }
    case 189:
        smoke_require(
            last_project_outcome_ == "opened" &&
                orbit_->input.propagator == OrbitPropagator::mission &&
                orbit_->session.maneuvers(scenario_->first(), scenario_->last())[0].name ==
                    "Renamed burn",
            "Reopening the project lost the applied mission");
        smoke_record("mission-edit-preserves-old-session-and-save-reopen-keeps-new-plan");
        set_selection(0);
        show_reports();
        report_dialog_->select_object("workflow-audit", application::ReportKind::cartesian_state);
        report_dialog_->findChild<QComboBox *>("reportFrame")->setCurrentIndex(0);
        report_dialog_->findChild<QDoubleSpinBox *>("reportStep")->setValue(60);
        report_dialog_->findChild<QPushButton *>("generateReport")->click();
        ++orbit_smoke_stage_;
        break;
    case 190: {
        if (!report_dialog_->findChild<QPushButton *>("generateReport")->isEnabled())
            break;
        const auto *model = report_dialog_->findChild<QTableView *>("reportTable")->model();
        const auto burns = orbit_->session.maneuvers(scenario_->first(), scenario_->last());
        const auto previous =
            smoke_cache_->session.maneuvers(scenario_->first(), scenario_->last());
        smoke_require(model->rowCount() == 11 && burns[0].delta_v_gcrf_mps[1] == 20 &&
                          previous[0].delta_v_gcrf_mps[1] == 10 &&
                          model->index(5, smoke_report_column(model, "state.vy"))
                                  .data(Qt::UserRole)
                                  .toDouble() == burns[0].after.velocity_mps[1],
                      "Regenerating the reopened mission report reused the old burn");
        smoke_record("mission-save-reopen-report-uses-updated-burn");
        workspace_->close_page(report_dialog_);
        seek_epoch(scenario_->first());
        fit_views();
        fit_pending_ = true;
        smoke_final_frames_ = 0;
        ++orbit_smoke_stage_;
        break;
    }
    case 191:
        if (++smoke_final_frames_ >= 4)
            ++orbit_smoke_stage_;
        break;
    default:
        throw std::logic_error("Stage does not belong to specialist_reports");
    }
}
