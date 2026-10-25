void Workbench::smoke_object_insertion() {
    auto input = orbit_ ? orbit_->input : OrbitInput{};
    switch (orbit_smoke_stage_) {
    case 198: {
        smoke_insert_counts_ = {satellites_.size(), stations_.size(), sensors_.size()};
        PreferencesDialog preferences(*network_settings_, eop_catalog_.get(), this,
                                      PreferencesPage::insertion);
        auto *default_method = preferences.findChild<QComboBox *>("preferencesInsertMethod0");
        default_method->setCurrentIndex(default_method->findData("defaults"));
        preferences.findChild<QCheckBox *>("preferencesKeepInsertOpen")->setChecked(true);
        preferences.findChild<QPushButton *>("savePreferences")->click();
        smoke_require(preferences.result() == QDialog::Accepted,
                      "Cannot save the central insertion preferences");
        show_insert_objects();
        insert_dialog_->select_type(InsertObjectType::satellite);
        auto *methods = insert_dialog_->findChild<QTreeWidget *>("insertObjectMethods");
        methods->setCurrentItem(methods->topLevelItem(1));
        auto *button = insert_dialog_->findChild<QPushButton *>("insertObject");
        button->click();
        smoke_require(busy() && !button->isEnabled(), "Insertion was not blocked while busy");
        button->click();
        ++orbit_smoke_stage_;
        break;
    }
    case 199: {
        smoke_require(
            satellites_.size() == smoke_insert_counts_[0] + 1 && insert_dialog_->isVisible() &&
                insert_dialog_->findChild<QLabel *>("insertObjectResult")->text() == "对象已插入。",
            "Continuous insertion duplicated a satellite or retained pending status");
        smoke_record("continuous-insert-publishes-once-and-clears-pending-status");
        insert_dialog_->select_type(InsertObjectType::ground_station);
        auto *methods = insert_dialog_->findChild<QTreeWidget *>("insertObjectMethods");
        methods->setCurrentItem(methods->topLevelItem(1));
        insert_dialog_->findChild<QPushButton *>("insertObject")->click();
        ++orbit_smoke_stage_;
        break;
    }
    case 200: {
        smoke_require(stations_.size() == smoke_insert_counts_[1] + 1,
                      "Default ground station was not inserted");
        insert_dialog_->select_type(InsertObjectType::sensor);
        insert_dialog_->set_parents(
            project_object_choices(project_snapshot(project_path_), {ObjectKind::satellite}),
            satellites_.back().orbit->input.object_id);
        auto *methods = insert_dialog_->findChild<QTreeWidget *>("insertObjectMethods");
        methods->setCurrentItem(methods->topLevelItem(1));
        timer_.stop();
        QApplication::processEvents();
        smoke_require(insert_dialog_->grab().save(smoke_dir_ + "/insert-sensor.png"),
                      "Cannot capture sensor insertion");
        insert_dialog_->resize(360, 300);
        for (int pass = 0; pass < 6; ++pass)
            QApplication::processEvents();
        smoke_require(insert_dialog_->grab().save(smoke_dir_ + "/insert-sensor-narrow.png"),
                      "Cannot capture narrow sensor insertion");
        timer_.start();
        insert_dialog_->findChild<QPushButton *>("insertObject")->click();
        ++orbit_smoke_stage_;
        break;
    }
    case 201:
        smoke_require(sensors_.size() == smoke_insert_counts_[2] + 1 &&
                          sensors_.back().parent_satellite_id ==
                              satellites_.back().orbit->input.object_id,
                      "Sensor insertion did not use its explicitly selected parent");
        smoke_record("insert-station-and-sensor-with-explicit-parent");
        insert_dialog_->close();
        quick_insert_actions_[0]->trigger();
        smoke_require(busy(), "Quick insertion did not use the stored satellite default");
        ++orbit_smoke_stage_;
        break;
    case 202: {
        smoke_require(satellites_.size() == smoke_insert_counts_[0] + 2,
                      "Quick satellite insertion did not finish");
        smoke_record("quick-insert-honors-per-type-preference");
        QTimer::singleShot(0, this, [] {
            if (auto *dialog = qobject_cast<QDialog *>(QApplication::activeModalWidget()))
                dialog->reject();
        });
        insert_object({InsertObjectType::satellite, InsertObjectMethod::orbit_wizard, {}});
        smoke_require(!busy() && satellites_.size() == smoke_insert_counts_[0] + 2,
                      "Cancelling the orbit wizard inserted an object");
        bool accepted = false;
        QTimer::singleShot(0, this, [&] {
            auto *wizard = findChild<QDialog *>("orbitWizard");
            if (!wizard)
                return;
            auto *presets = wizard->findChild<QComboBox *>("orbitWizardPreset");
            auto *preview = static_cast<OrbitDesignPreview *>(
                wizard->findChild<QWidget *>("orbitDesignPreview"));
            auto *map = static_cast<GroundMap *>(wizard->findChild<QWidget *>("wizardGroundMap"));
            for (int index = 0; index < presets->count(); ++index) {
                presets->setCurrentIndex(index);
                QElapsedTimer preview_wait;
                preview_wait.start();
                while ((!preview->ready() || !map->tracks_ready() || map->tracks_transitioning()) &&
                       preview_wait.elapsed() < 5000) {
                    QApplication::processEvents();
                    QThread::msleep(5);
                }
                smoke_require(preview->ready(), "Orbit design preview did not finish");
                smoke_require(map->tracks_ready() && !map->tracks_transitioning(),
                              "Orbit design refinement did not finish painting");
                smoke_require(wizard->findChild<QLabel *>("orbitWizardError")->text().isEmpty(),
                              "A standard orbit design has invalid default parameters");
                smoke_require(wizard->grab().save(smoke_dir_ + "/orbit-wizard-" +
                                                  QString::number(index) + ".png"),
                              "Cannot capture orbit design layout");
            }
            // Exercise continuous changes, not just a settled screenshot: stale
            // cancellation used to prevent every frame during a sustained drag.
            presets->setCurrentIndex(0);
            wizard->findChild<QComboBox *>("wizardPreviewRevolutions")->setCurrentIndex(1);
            {
                WorkbenchProfile interaction_profile(smoke_dir_ + "/orbit-preview-profile");
                for (const char *parameter : {"wizardRaan", "wizardAltitude"}) {
                    auto *field = wizard->findChild<QuantityEditor *>(parameter);
                    for (int interval : {16, 8, 4}) {
                        interaction_profile.phase(
                            QStringLiteral("%1-%2ms").arg(parameter).arg(interval), true);
                        QElapsedTimer drag, gap;
                        drag.start();
                        gap.start();
                        int inputs = 0, drawn = 0;
                        double maximum_gap = 0;
                        QPointF last;
                        while (drag.elapsed() < 1600) {
                            if (drag.elapsed() >= inputs * interval) {
                                const ProfileScope edit("input_handler_ms");
                                field->set_value((field->unit() == Unit::degree ? 20 : 500) +
                                                     inputs % 100,
                                                 field->unit());
                                ++inputs;
                            }
                            QApplication::processEvents();
                            if (map->track_count() && map->tracks_ready()) {
                                const auto point = map->track_coordinates(0).back();
                                if (point != last) {
                                    ++drawn;
                                    last = point;
                                    const double milliseconds = gap.nsecsElapsed() * 1e-6;
                                    maximum_gap = std::max(maximum_gap, milliseconds);
                                    interaction_profile.record("drawable_gap_ms", milliseconds);
                                    gap.start();
                                }
                            }
                            QThread::msleep(1);
                        }
                        smoke_require(drawn > 1,
                                      "Orbit preview stalled throughout continuous input");
                        interaction_report_.append(
                            QJsonObject{{"kind", "orbit-wizard-continuous-preview"},
                                        {"passed", true},
                                        {"parameter", parameter},
                                        {"input_interval_ms", interval},
                                        {"inputs", inputs},
                                        {"drawable_updates", drawn},
                                        {"maximum_drawable_gap_ms", maximum_gap}});
                    }
                }
                interaction_profile.phase("refinement", true);
                smoke_require(
                    wizard->grab().save(smoke_dir_ + "/orbit-wizard-refinement-before.png"),
                    "Cannot capture the interactive orbit curve");
                bool captured_transition = false;
                QElapsedTimer refinement_wait;
                refinement_wait.start();
                while ((!preview->ready() || !map->tracks_ready() || map->tracks_transitioning()) &&
                       refinement_wait.elapsed() < 5000) {
                    QApplication::processEvents();
                    auto *animation = map->findChild<QVariantAnimation *>("groundMapRefinement");
                    if (!captured_transition && animation &&
                        animation->state() == QAbstractAnimation::Running &&
                        animation->currentTime() >= 60) {
                        captured_transition =
                            wizard->grab().save(smoke_dir_ + "/orbit-wizard-refinement-during.png");
                    }
                    QThread::msleep(1);
                }
                smoke_require(preview->ready() && map->tracks_ready() &&
                                  !map->tracks_transitioning(),
                              "Orbit refinement failed to settle");
                smoke_require(
                    wizard->grab().save(smoke_dir_ + "/orbit-wizard-refinement-after.png"),
                    "Cannot capture the settled orbit curve");
                interaction_report_.append(
                    QJsonObject{{"kind", "orbit-wizard-refinement"},
                                {"passed", true},
                                {"captured_intermediate_frame", captured_transition}});
                interaction_profile.finish({{"device_pixel_ratio", wizard->devicePixelRatioF()}});
            }
            presets->setCurrentIndex(7);
            wizard->resize(360, 300);
            for (int iteration = 0; iteration < 5; ++iteration)
                QApplication::processEvents();
            smoke_require(wizard->grab().save(smoke_dir_ + "/orbit-wizard-narrow.png"),
                          "Cannot capture narrow orbit design layout");
            wizard->resize(980, 590);
            QApplication::processEvents();
            accepted = wizard->grab().save(smoke_dir_ + "/orbit-wizard.png");
            wizard->findChild<QPushButton *>("acceptOrbitWizard")->click();
        });
        insert_object({InsertObjectType::satellite, InsertObjectMethod::orbit_wizard, {}});
        smoke_require(accepted && !busy() && !editor_target_ &&
                          satellites_.size() == smoke_insert_counts_[0] + 2,
                      "The orbit wizard failed to open a draft before publication");
        smoke_record("orbit-wizard-cancel-and-accept-preserve-draft-boundary");
        editor_->findChild<QPushButton *>("resetOrbit")->click();
        properties_dialog_->close();
        workspace_->select_view(0);
        fit_views();
        fit_pending_ = true;
        smoke_final_frames_ = 0;
        ++orbit_smoke_stage_;
        break;
    }
    case 203: {
        const bool native_disabled = QApplication::testAttribute(Qt::AA_DontUseNativeDialogs);
        QApplication::setAttribute(Qt::AA_DontUseNativeDialogs, true);
        QTimer::singleShot(0, this, [] {
            if (auto *dialog = qobject_cast<QFileDialog *>(QApplication::activeModalWidget()))
                dialog->reject();
        });
        insert_object({InsertObjectType::satellite, InsertObjectMethod::tle_file, {}});
        smoke_require(!busy() && satellites_.size() == smoke_insert_counts_[0] + 2,
                      "Cancelling TLE import changed the scene");
        const QString tle =
            "1 00005U 58002B   00179.78495062  .00000023  00000-0  28098-4 0  4753\n"
            "2 00005  34.2682 348.7242 1859667 331.7664  19.3264 10.82419157413667";
        const auto path = smoke_dir_ + "/single-satellite.tle";
        QFile file(path);
        smoke_require(file.open(QIODevice::WriteOnly), "Cannot create TLE import fixture");
        file.write(tle.toUtf8());
        file.close();
        QTimer::singleShot(0, this, [path] {
            if (auto *dialog = qobject_cast<QFileDialog *>(QApplication::activeModalWidget())) {
                dialog->selectFile(path);
                QMetaObject::invokeMethod(dialog, "accept", Qt::QueuedConnection);
            }
        });
        insert_object({InsertObjectType::satellite, InsertObjectMethod::tle_file, {}});
        QApplication::setAttribute(Qt::AA_DontUseNativeDialogs, native_disabled);
        smoke_require(!busy() && !editor_target_ &&
                          editor_->findChild<QPlainTextEdit *>("tleInput")->toPlainText() == tle &&
                          editor_->findChild<QComboBox *>("orbitPropagator")->currentIndex() ==
                              static_cast<int>(OrbitPropagator::sgp4) &&
                          satellites_.size() == smoke_insert_counts_[0] + 2,
                      "TLE file import did not open an uncommitted SGP4 draft");
        smoke_record("tle-file-import-and-cancel-preserve-draft-boundary");
        properties_dialog_->close();
        workspace_->select_view(0);
        ++orbit_smoke_stage_;
        break;
    }
    default:
        throw std::logic_error("Stage does not belong to object_insertion");
    }
}
