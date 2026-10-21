void Workbench::smoke_hpop_apply() {
    auto input = orbit_ ? orbit_->input : OrbitInput{};
    switch (orbit_smoke_stage_) {
    case 158: {
        application::Project project{
            "HPOP Apply audit", scenario_->first(),
            core::Epoch{scenario_->first().tai_nanoseconds() + 3600'000'000'000LL}};
        for (int i = 0; i < 2; ++i) {
            application::ProjectSatellite satellite;
            satellite.orbit.object_id = "hpop-" + std::to_string(i);
            satellite.orbit.first = project.first;
            satellite.orbit.last = project.last;
            satellite.orbit.initial.epoch = project.first;
            satellite.orbit.propagator = OrbitPropagator::hpop;
            project.satellites.push_back(satellite);
        }
        start_project(std::move(project), smoke_dir_ + "/apply-audit.orbitforge");
        ++orbit_smoke_stage_;
        break;
    }
    case 159: {
        smoke_require(last_project_outcome_ == "opened", "HPOP audit scene did not open");
        QFile fixture(smoke_dir_ + "/apply-gravity.gfc");
        smoke_require(fixture.open(QIODevice::WriteOnly), "Cannot write gravity audit fixture");
        fixture.write("product_type gravity_field\nmodelname ApplyAudit\n"
                      "earth_gravity_constant 3.986004415e14\nradius 6378136.3\n"
                      "max_degree 2\ntide_system tide_free\nend_of_head\n"
                      "gfc 0 0 1 0\ngfc 2 0 -0.000484165371736 0\n"
                      "gfc 2 1 0 0\ngfc 2 2 0 0\n");
        fixture.close();
        const auto supplied = qEnvironmentVariable("ORBITFORGE_APPLY_GRAVITY_FILE");
        const auto source = supplied.isEmpty() ? fixture.fileName() : supplied;
        runtime::CatalogGravity installed;
        if (smoke_gravity_id_ == "egm2008") {
            const auto files = eop_catalog_->data().gravity_models();
            const auto found =
                std::ranges::find(files, "egm2008", &runtime::CatalogGravity::selection);
            smoke_require(found != files.end(), "Startup gravity fixture disappeared");
            installed = *found;
        } else {
            installed = eop_catalog_->data().install_gravity(
                std::filesystem::path(source.toStdWString()), smoke_gravity_id_);
        }
        set_selection(0);
        open_properties();
        auto changed = satellites_[0].orbit->input;
        changed.hpop.gravity_model = smoke_gravity_id_;
        changed.hpop.gravity_mu = installed.info.mu;
        changed.hpop.gravity_degree = std::min(21, installed.info.degree);
        changed.hpop.gravity_order = std::min(changed.hpop.gravity_degree, installed.info.order);
        editor_->set_input(changed);
        // A dependency explicitly added by Apply must not need a second consent.
        QTimer::singleShot(0, [] {
            if (auto *box = qobject_cast<QMessageBox *>(QApplication::activeModalWidget()))
                box->reject();
        });
        smoke_apply_elapsed_.start();
        editor_->submit();
        ++orbit_smoke_stage_;
        break;
    }
    case 160: {
        smoke_require(editor_->findChild<QLabel *>("orbitResult")->text() ==
                              QStringLiteral("轨道已更新。") &&
                          editor_->findChild<QPushButton *>("applyOrbit")->isEnabled(),
                      "HPOP Apply completed but retained its pending status");
        smoke_require(properties_dialog_->grab().save(smoke_dir_ + "/hpop-apply-complete.png"),
                      "Cannot capture HPOP completion feedback");
        smoke_record("hpop-apply-publishes-completion-status");
        smoke_require(last_orbit_outcome_ == "applied" && gravity_.contains(smoke_gravity_id_),
                      "Explicit HPOP gravity selection prompted for data consent or failed");
        smoke_require(workspace_->contains(properties_dialog_) && !editor_->has_unapplied_changes(),
                      "HPOP Apply closed the property page or failed to accept its input");
        smoke_require(satellites_[1].orbit->session.provenance().input_revision == 1,
                      "Selecting one satellite's model recomputed the other satellite");
        smoke_record("hpop-explicit-model-applies-without-data-conflict");
        interaction_report_.append(QJsonObject{{"kind", smoke_gravity_id_ == "egm2008"
                                                            ? "hpop-preloaded-first-apply-timing"
                                                            : "hpop-cold-apply-timing"},
                                               {"elapsed_ms", smoke_apply_elapsed_.elapsed()},
                                               {"passed", true}});
        smoke_gravity_ = gravity_.at(smoke_gravity_id_);
        if (smoke_gravity_id_ == "egm2008") {
            smoke_require(smoke_gravity_ == smoke_startup_gravity_.lock(),
                          "First HPOP application did not reuse startup gravity across scenes");
            smoke_record("hpop-first-apply-shares-startup-resource-after-scene-changes");
        }
        set_selection(0);
        open_properties();
        editor_->set_state_mode(0);
        auto *velocity = editor_->findChild<QuantityEditor *>("velocityY");
        velocity->set_value(velocity->value(Unit::metres_per_second) + 1, Unit::metres_per_second);
        smoke_apply_elapsed_.restart();
        editor_->submit();
        ++orbit_smoke_stage_;
        break;
    }
    case 161: {
        smoke_require(last_orbit_outcome_ == "applied" &&
                          gravity_.at(smoke_gravity_id_) == smoke_gravity_,
                      "Editing HPOP reloaded an unchanged gravity resource");
        smoke_require(satellites_[0].orbit->session.provenance().input_revision == 3 &&
                          satellites_[1].orbit->session.provenance().input_revision == 1,
                      "HPOP update did not preserve the unaffected numerical session");
        smoke_record("hpop-apply-reuses-resource-and-unaffected-session");
        interaction_report_.append(QJsonObject{{"kind", "hpop-warm-apply-timing"},
                                               {"elapsed_ms", smoke_apply_elapsed_.elapsed()},
                                               {"passed", true}});
        set_selection(0);
        open_properties();
        const auto revision = scenario_->revision();
        const auto current = satellites_[0].orbit;
        editor_->submit();
        smoke_require(last_orbit_outcome_ == "unchanged" && !busy() &&
                          scenario_->revision() == revision && satellites_[0].orbit == current,
                      "Unchanged Apply started work or revised the scenario");
        smoke_require(editor_->findChild<QLabel *>("orbitResult")->text() ==
                          QStringLiteral("没有需要应用的更改。"),
                      "Unchanged Apply left a previous task status");
        smoke_record("unchanged-apply-does-not-compute-or-confirm-data");
        properties_dialog_->close();
        save_project_to(smoke_dir_ + "/apply-audit.orbitforge");
        smoke_final_frames_ = 0;
        ++orbit_smoke_stage_;
        break;
    }
    case 162: {
        if (smoke_final_frames_++ == 0) {
            const auto captured_report = report_input();
            smoke_require(captured_report.sessions == orbit_sessions_,
                          "Report did not capture the published scientific cache");
            const auto &orbit = captured_report.project.satellites.front().orbit;
            const application::ProjectDocument document{captured_report.project};
            const auto analysis =
                runtime::prepare(document, captured_report.data, captured_report.sessions);
            const std::array epochs{orbit.first, orbit.last};
            int displayed_checks = 0, report_checks = 0;
            const auto displayed =
                satellites_[0].orbit->session.sample(epochs, [&] { ++displayed_checks; });
            const auto shared = captured_report.sessions->acquire(
                orbit, document.snapshot().revision, captured_report.data.time().get(),
                captured_report.data.force_environment(orbit));
            (void)shared.sample(epochs, [&] { ++report_checks; });
            const auto states = analysis.states(document.objects().get(orbit.object_id), epochs,
                                                core::FrameId::gcrf);
            smoke_require(
                states.states().front().position_m == displayed.states().front().position_m &&
                    states.states().back().position_m == displayed.states().back().position_m,
                "First report differs from the published scientific solution");
            smoke_require(report_checks == displayed_checks,
                          "First report repeated the prepared HPOP integration");
            show_reports();
            report_dialog_->select_object(orbit.object_id,
                                          application::ReportKind::cartesian_state);
            smoke_choose_report(
                QStringLiteral("builtin:%1")
                    .arg(static_cast<int>(application::ReportKind::cartesian_state)));
            report_dialog_->findChild<QDoubleSpinBox *>("reportStep")->setValue(60);
            smoke_apply_elapsed_.restart();
            report_dialog_->findChild<QPushButton *>("generateReport")->click();
            break;
        }
        if (!report_dialog_->findChild<QPushButton *>("generateReport")->isEnabled())
            break;
        smoke_require(report_dialog_->findChild<QTableView *>("reportTable")->model()->rowCount() ==
                          61,
                      "First HPOP report did not use its independent sampling interval");
        interaction_report_.append(QJsonObject{{"kind", "hpop-first-report-timing"},
                                               {"elapsed_ms", smoke_apply_elapsed_.elapsed()},
                                               {"passed", true}});
        smoke_record("hpop-first-report-shares-prepared-science");
        workspace_->close_page(report_dialog_);
        ++orbit_smoke_stage_;
        break;
    }
    case 163: {
        application::Project project{"Apply navigation audit", scenario_->first(),
                                     scenario_->last()};
        application::ProjectSatellite satellite;
        satellite.orbit.object_id = "hpop-builtin";
        satellite.orbit.propagator = OrbitPropagator::hpop;
        satellite.orbit.initial.epoch = project.first;
        satellite.orbit.first = project.first;
        satellite.orbit.last = project.last;
        project.satellites.push_back(satellite);
        start_project(std::move(project), smoke_dir_ + "/apply-navigation.orbitforge");
        ++orbit_smoke_stage_;
        break;
    }
    case 164: {
        smoke_require(last_project_outcome_ == "opened" && gravity_.empty(),
                      "Built-in HPOP audit scene did not open");
        set_selection(0);
        open_properties();
        smoke_wait_frame_ = frame_count_ + 3;
        ++orbit_smoke_stage_;
        break;
    }
    case 165: {
        if (frame_count_ < smoke_wait_frame_)
            return;
        smoke_view_epoch_ = core::Epoch{scenario_->first().tai_nanoseconds() + 1200'000'000'123LL};
        seek_epoch(smoke_view_epoch_);
        slots_[0]->navigation->set_viewpoint(vsg::LookAt::create(
            vsg::dvec3{19e6, 7e6, 4e6}, vsg::dvec3{1e6, 2e6, 0}, vsg::dvec3{0, 0, 1}));
        const auto *look = dynamic_cast<vsg::LookAt *>(slots_[0]->camera->viewMatrix.get());
        smoke_camera_ = {look->eye, look->center, look->up};
        smoke_cache_ = satellites_[0].orbit;
        auto changed = satellites_[0].orbit->input;
        changed.step_seconds = 30;
        editor_->set_input(changed);
        editor_->submit();
        smoke_require(candidate_.valid(),
                      "Display sampling edit did not use the single-orbit path");
        ++orbit_smoke_stage_;
        break;
    }
    case 166: {
        smoke_require(editor_->findChild<QLabel *>("orbitResult")->text() ==
                          QStringLiteral("轨道已更新。"),
                      "Single-orbit Apply retained a pending status");
        const auto *look = dynamic_cast<vsg::LookAt *>(slots_[0]->camera->viewMatrix.get());
        smoke_require(last_orbit_outcome_ == "applied",
                      qPrintable(editor_->findChild<QLabel *>("orbitResult")->text()));
        smoke_require(satellites_[0].orbit->session.provenance().input_revision ==
                              smoke_cache_->session.provenance().input_revision &&
                          satellites_[0].orbit->input.step_seconds == 30,
                      "Display sampling edit recomputed HPOP");
        smoke_require(snapshot_.epoch == smoke_view_epoch_,
                      "Display sampling edit reset the precise scene time");
        smoke_require(look->eye == smoke_camera_[0] && look->center == smoke_camera_[1] &&
                          look->up == smoke_camera_[2],
                      "Display sampling edit reset the camera");
        smoke_record("single-orbit-display-edit-reuses-session-and-keeps-navigation");
        smoke_cache_ = satellites_[0].orbit;
        open_properties();
        auto changed = satellites_[0].orbit->input;
        changed.initial.velocity_mps[1] += 1;
        editor_->set_input(changed);
        editor_->submit();
        ++orbit_smoke_stage_;
        break;
    }
    case 167: {
        const auto *look = dynamic_cast<vsg::LookAt *>(slots_[0]->camera->viewMatrix.get());
        smoke_require(last_orbit_outcome_ == "applied" &&
                          satellites_[0].orbit->session.provenance().input_revision ==
                              smoke_cache_->session.provenance().input_revision + 1 &&
                          snapshot_.epoch == smoke_view_epoch_ && look->eye == smoke_camera_[0] &&
                          look->center == smoke_camera_[1] && look->up == smoke_camera_[2],
                      "Physical orbit edit failed to retain time/camera");
        smoke_record("single-orbit-physical-edit-keeps-navigation");
        smoke_cache_ = satellites_[0].orbit;
        set_playing(true);
        auto project = project_snapshot(project_path_);
        project.ground_stations.push_back({"audit-station", "", {1.2, 0.6, 350}});
        start_project(std::move(project), project_path_, true, 0);
        smoke_require(playing_, "Project edit paused playback before publishing");
        // A seek after submission must survive the background result publication.
        set_playing(false);
        smoke_view_epoch_ = core::Epoch{scenario_->first().tai_nanoseconds() + 1800'000'000'321LL};
        seek_epoch(smoke_view_epoch_);
        ++orbit_smoke_stage_;
        break;
    }
    case 168: {
        const auto *look = dynamic_cast<vsg::LookAt *>(slots_[0]->camera->viewMatrix.get());
        smoke_require(last_project_outcome_ == "opened" && stations_.size() == 1 && !playing_ &&
                          snapshot_.epoch == smoke_view_epoch_ && look->eye == smoke_camera_[0] &&
                          look->center == smoke_camera_[1] && look->up == smoke_camera_[2] &&
                          satellites_[0].orbit->session.provenance().input_revision ==
                              smoke_cache_->session.provenance().input_revision,
                      "Project edit discarded navigation made during preparation");
        smoke_record("project-edit-keeps-latest-seek-and-camera");
        set_playing(true);
        auto project = project_snapshot(project_path_);
        project.ground_stations[0].minimum_elevation_rad = 0.1;
        start_project(std::move(project), project_path_, true, 0);
        smoke_require(playing_, "Constraint edit paused playback");
        ++orbit_smoke_stage_;
        break;
    }
    case 169: {
        smoke_require(last_project_outcome_ == "opened" && playing_ &&
                          stations_[0].config.minimum_elevation_rad == 0.1,
                      "Successful project edit stopped playback");
        smoke_record("project-edit-keeps-playback");
        auto project = project_snapshot(project_path_);
        project.ground_stations[0].minimum_elevation_rad = 0.2;
        start_project(std::move(project), project_path_, true, 0);
        project_cancellation_->store(true);
        ++orbit_smoke_stage_;
        break;
    }
    case 170:
        smoke_require(last_project_outcome_ == "cancelled" && playing_ &&
                          stations_[0].config.minimum_elevation_rad == 0.1,
                      "Cancelled project edit changed playback or applied its draft");
        smoke_record("cancelled-project-edit-keeps-playback-and-applied-state");
        set_playing(false);
        save_project_to(smoke_dir_ + "/apply-navigation.orbitforge");
        // Navigation assertions above deliberately leave the globe off-center.
        // Restore a fitted view for the final whole-scene pixel checks.
        fit_views();
        fit_pending_ = true;
        smoke_final_frames_ = 0;
        ++orbit_smoke_stage_;
        break;
    case 171:
        if (++smoke_final_frames_ >= 4)
            ++orbit_smoke_stage_;
        break;
    default:
        throw std::logic_error("Stage does not belong to hpop_apply");
    }
}
