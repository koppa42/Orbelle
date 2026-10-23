void Workbench::smoke_data_catalog() {
    auto input = orbit_ ? orbit_->input : OrbitInput{};
    switch (orbit_smoke_stage_) {
    case 86: {
        eop_catalog_->set_preferred_product(DataProduct::finals2000a);
        smoke_require(last_project_outcome_ == "opened" && satellites_.size() == 2 &&
                          satellites_[0].name() == QStringLiteral("观测卫星 A"),
                      "Object names were not restored from the project");
        const auto resource = test_resources_ + (eop_ == eop_current_ ? "/eop-current" : "/eop");
        QFile manifest(resource + "/manifest.json");
        smoke_require(manifest.open(QIODevice::ReadOnly), "Cannot open smoke EOP source");
        auto json = QJsonDocument::fromJson(manifest.readAll()).object();
        json["snapshot_id"] = "smoke-imported-" + QString::fromStdString(eop_->data_info().id);
        const auto source = smoke_dir_ + "/import-source";
        smoke_require(QDir().mkpath(source), "Cannot create import fixture");
        const auto filename = json["data_file"].toString();
        smoke_require(QFile::copy(resource + "/" + filename, source + "/" + filename),
                      "Cannot copy EOP fixture");
        QFile copy(source + "/manifest.json");
        smoke_require(copy.open(QIODevice::WriteOnly), "Cannot write EOP fixture");
        copy.write(QJsonDocument(json).toJson());
        copy.close();
        QCoreApplication::setAttribute(Qt::AA_DontUseNativeDialogs, true);
        QTimer::singleShot(0, [&, source] {
            auto *dialog = qobject_cast<QDialog *>(QApplication::activeModalWidget());
            smoke_require(dialog != nullptr, "Missing EOP dialog");
            auto *details = dialog->findChild<QLabel *>("dataInventoryDetails");
            auto *automatic = dialog->findChild<QCheckBox *>("dataAutoUpdate");
            auto *inventory = dialog->findChild<QTreeWidget *>("dataInventory");
            auto *priority = dialog->findChild<QListWidget *>("eopSourcePriority");
            auto *pages = dialog->findChild<QTabWidget *>("dataPages");
            smoke_require(pages && details && automatic && automatic->isChecked() && inventory &&
                              inventory->topLevelItemCount() == 6 && priority &&
                              inventory->topLevelItem(0)->childCount() == 2 &&
                              priority->isVisible() &&
                              !dialog->findChild<QComboBox *>("eopSnapshots") &&
                              !dialog->findChild<QPushButton *>("setDefaultEop") &&
                              !dialog->findChild<QPushButton *>("applyEopSnapshot"),
                          "Data inventory must show current data without version/default controls");
            auto *group = inventory->topLevelItem(0);
            auto *finals = group->child(0);
            auto *c04 = group->child(1);
            auto *utc = inventory->topLevelItem(1);
            smoke_require(
                priority->model()->moveRow({}, 1, {}, 0) &&
                    eop_catalog_->preferred_product() == DataProduct::c04 &&
                    inventory->currentItem() == group && scenario_->data_lock() == smoke_data_lock_,
                "Reordering sources must persist priority without changing the active scene");
            smoke_require(priority->model()->moveRow({}, 1, {}, 0) &&
                              eop_catalog_->preferred_product() == DataProduct::finals2000a,
                          "Cannot restore default source priority");
            QCoreApplication::sendPostedEvents(nullptr, QEvent::LayoutRequest);
            smoke_require(dialog->grab().save(smoke_dir_ + "/data-center-priority.png"),
                          "Cannot save source priority page");
            inventory->setCurrentItem(finals);
            smoke_require(dialog->findChild<QLabel *>("dataAcquiredAt")
                              ->text()
                              .contains(QStringLiteral("随程序提供")),
                          "Missing acquisition information");
            smoke_require(finals->text(0).contains(QStringLiteral("内置示例")) &&
                              dialog->findChild<QLabel *>("dataLastChecked")
                                  ->text()
                                  .contains(QStringLiteral("尚未检查")),
                          "Bundled samples must not claim a full download or a successful check");
            auto *frequency = dialog->findChild<QComboBox *>("dataUpdateFrequency");
            smoke_require(frequency, "Missing automatic update interval");
            smoke_require(!automatic->isVisible(),
                          "Overview must not expose advanced update settings");
            pages->setCurrentIndex(1);
            frequency->setCurrentIndex(frequency->findData(6));
            smoke_require(eop_catalog_->update_interval_hours() == 6,
                          "Automatic update frequency was not persisted");
            frequency->setCurrentIndex(frequency->findData(24));
            pages->setCurrentIndex(0);
            QCoreApplication::sendPostedEvents(nullptr, QEvent::LayoutRequest);
            smoke_require(dialog->grab().save(smoke_dir_ + "/data-center-sample.png"),
                          "Cannot save sample installation state");
            inventory->setCurrentItem(utc);
            smoke_require(
                dialog->findChild<QLabel *>("dataValidUntil")->text().contains("2027-06-28"),
                "Missing UTC expiry information");
            auto *allow = dialog->findChild<QCheckBox *>("allowEopExtrapolation");
            auto *days = dialog->findChild<QSpinBox *>("eopExtrapolationDays");
            smoke_require(allow && days && !allow->isVisible() && !priority->isVisible(),
                          "UTC must not expose EOP extrapolation controls");
            QCoreApplication::sendPostedEvents(nullptr, QEvent::LayoutRequest);
            smoke_require(dialog->grab().save(smoke_dir_ + "/data-center-utc.png"),
                          "Cannot save UTC data details");
            inventory->setCurrentItem(finals);
            pages->setCurrentIndex(1);
            smoke_require(!allow->isChecked() && !days->isEnabled(),
                          "Extrapolation should default off");
            allow->setChecked(true);
            days->setValue(2);
            smoke_require(eop_catalog_->extrapolation_days(0) == 2,
                          "Extrapolation limit was not saved");
            inventory->setCurrentItem(c04);
            smoke_require(!allow->isChecked(), "EOP policies must be independent by product");
            inventory->setCurrentItem(finals);
            pages->setCurrentIndex(1);
            smoke_require(allow->isChecked() && days->value() == 2,
                          "Lost extrapolation setting on selection change");
            QCoreApplication::sendPostedEvents(nullptr, QEvent::LayoutRequest);
            smoke_require(dialog->grab().save(smoke_dir_ + "/data-center-extrapolation.png"),
                          "Cannot save EOP policy details");
            allow->setChecked(false);
            inventory->setCurrentItem(c04);
            smoke_record("data-center-per-product-extrapolation-controls");
            QTimer::singleShot(0, [source] {
                if (auto *file = qobject_cast<QFileDialog *>(QApplication::activeModalWidget())) {
                    file->selectFile(source + "/manifest.json");
                    QMetaObject::invokeMethod(file, "accept");
                }
            });
            pages->setCurrentIndex(2);
            dialog->findChild<QPushButton *>("importEopSnapshot")->click();
            smoke_require(eop_catalog_->providers().size() == 3, "Snapshot import failed");
            smoke_require(eop_->data_info().id.find("smoke-imported-") != 0,
                          "Import implicitly switched the active data");
            const auto imported_row =
                eop_->data_info().interpolation.starts_with("finals2000a") ? 0 : 1;
            const auto imported = eop_catalog_->inventory()[static_cast<std::size_t>(imported_row)];
            smoke_require(inventory->topLevelItemCount() == 6 &&
                              inventory->currentItem() == group->child(imported_row) &&
                              imported.provider_index == 2 && imported.acquired_at &&
                              imported.acquisition == runtime::DataAcquisition::imported &&
                              dialog->findChild<QLabel *>("dataAcquiredAt")->text() ==
                                  QDateTime::fromSecsSinceEpoch(*imported.acquired_at)
                                      .toLocalTime()
                                      .toString("yyyy-MM-dd HH:mm"),
                          "Import must select its source with an acquisition timestamp");
            dialog->grab().save(smoke_dir_ + "/data-center-inventory.png");
            pages->setCurrentIndex(2);
            dialog->grab().save(smoke_dir_ + "/eop-data-dialog.png");
            inventory->setCurrentItem(finals);
            automatic->setChecked(false);
            automatic->setChecked(true);
            smoke_require(eop_catalog_->automatic_updates() && !eop_catalog_->downloading(),
                          "Update policy was not saved or unexpectedly started a download");
            automatic->setChecked(false);
            smoke_require(scenario_->data_lock() == smoke_data_lock_,
                          "Refreshing the data inventory changed the active data lock");
            smoke_record("data-inventory-acquisition-expiry-and-no-history-controls");
            dialog->reject();
        });
        eop_action_->trigger();
        smoke_answer_data(true);
        start_project(project_snapshot(project_path_), project_path_);
        project_cancellation_->store(true);
        ++orbit_smoke_stage_;
        break;
    }
    case 87:
        smoke_require(last_project_outcome_ == "cancelled" && !project_dirty_ &&
                          scenario_->data_lock() == smoke_data_lock_ &&
                          eop_->data_info().id.find("smoke-imported-") != 0,
                      "Cancelled reload changed the current calculation data");
        smoke_record("data-update-keeps-active-result-and-cancelled-reload");
        set_selection(0);
        open_properties();
        smoke_cache_ = orbit_;
        {
            auto edited_input = satellites_[0].orbit->input;
            edited_input.initial.position_m[0] *= 1.0001;
            editor_->set_input(edited_input);
            smoke_answer_data(false);
            editor_->submit();
            smoke_require(!busy() && orbit_ == smoke_cache_ && last_orbit_outcome_ == "cancelled" &&
                              scenario_->data_lock() == smoke_data_lock_,
                          "Declining recalculation changed the scene");
            smoke_record("recalculation-data-change-decline-preserves-scene");
            smoke_answer_data(true);
            editor_->submit();
        }
        ++orbit_smoke_stage_;
        break;
    case 88: {
        smoke_require(
            last_project_outcome_ == "opened" && project_dirty_ &&
                scenario_->data_lock() != smoke_data_lock_ &&
                eop_->data_info().id.find("smoke-imported-") == 0 &&
                last_orbit_outcome_ == "applied" &&
                orbit_->session.provenance().input_revision ==
                    smoke_cache_->session.provenance().input_revision + 1 &&
                orbit_->input.initial.position_m != smoke_cache_->input.initial.position_m &&
                std::all_of(satellites_.begin(), satellites_.end(),
                            [](const auto &satellite) {
                                return satellite.orbit->eop_source_id.find("smoke-imported-") !=
                                       std::string::npos;
                            }),
            "Recalculation did not use current data consistently for every object");
        smoke_record("recalculation-uses-current-data-and-rebuilds-all-tracks");
        save_project_to(smoke_dir_ + "/imported-data.orbelle");
        // Recreate the catalog as on a fresh process; source files are no longer needed.
        QFile changed(smoke_dir_ + "/import-source/manifest.json");
        smoke_require(changed.open(QIODevice::WriteOnly | QIODevice::Truncate),
                      "Cannot change import source");
        changed.write("removed original source");
        changed.close();
        eop_catalog_ = std::make_unique<EopCatalog>(
            eop_catalog_->store(),
            std::vector<std::filesystem::path>{
                std::filesystem::path(test_resources_.toStdWString()) / "eop-current/manifest.json",
                std::filesystem::path(test_resources_.toStdWString()) / "eop/manifest.json"},
            std::filesystem::path{}, false);
        editor_->data_catalog = eop_catalog_.get();
        smoke_require(eop_catalog_->providers().size() == 3 && eop_catalog_->warnings().empty() &&
                          eop_catalog_->current_indices().size() == 2 &&
                          std::find(eop_catalog_->current_indices().begin(),
                                    eop_catalog_->current_indices().end(),
                                    2) != eop_catalog_->current_indices().end() &&
                          !eop_catalog_->automatic_updates(),
                      "Imported data did not survive catalog reconstruction");
        start_project(
            application::load_project(std::filesystem::path(project_path_.toStdWString())),
            project_path_);
        ++orbit_smoke_stage_;
        break;
    }
    case 89:
        smoke_require(last_project_outcome_ == "opened" && !project_dirty_ &&
                          eop_->data_info().id.find("smoke-imported-") == 0 &&
                          satellites_[0].name() == QStringLiteral("观测卫星 A"),
                      "Reopening could not use the current installed EOP data");
        smoke_record("data-catalog-reload-uses-current-data-after-source-removal");
        smoke_require(
            scenario_->data_lock() == application::eop_data_lock(eop_->data_info()) &&
                scenario_->data_lock() ==
                    application::load_project(std::filesystem::path(project_path_.toStdWString()))
                        .data_lock(),
            "Restored scenario data lock differs from its file or active providers");
        open_scenario_properties();
        smoke_require(findChild<QLineEdit *>("scenarioDataHash")->text().toStdString() ==
                          scenario_->data_lock().sha256(),
                      "Scenario properties did not expose the bound data hash");
        scenario_properties_->close();
        smoke_record("calculation-data-provenance-survives-save-and-reopen");
        save_workbench_image("objects-and-data-restored.png");
        ++orbit_smoke_stage_;
        break;
    case 90: {
        const auto index = eop_catalog_->preferred_index();
        eop_catalog_->set_extrapolation_days(index, 2);
        const auto last = eop_catalog_->providers().at(index)->data_info().last_epoch;
        const core::Epoch first{last.tai_nanoseconds() + 3'600'000'000'000};
        application::Project extrapolated{"EOP extrapolation", first,
                                          core::Epoch{first.tai_nanoseconds() + 600'000'000'000}};
        application::ProjectSatellite satellite;
        satellite.orbit.initial.epoch = first;
        satellite.orbit.first = extrapolated.first;
        satellite.orbit.last = extrapolated.last;
        extrapolated.satellites.push_back(std::move(satellite));
        start_project(std::move(extrapolated), smoke_dir_ + "/extrapolated.orbelle");
        ++orbit_smoke_stage_;
        break;
    }
    case 91: {
        smoke_require(last_project_outcome_ == "opened" && satellites_.size() == 1 &&
                          eop_->data_info().extrapolation_days == 2 &&
                          satellites_[0].orbit->eop_source_id.find(
                              "extrapolation=endpoint-linear-tai-v1") != std::string::npos,
                      "Extrapolated EOP could not prepare an actual orbit scene");
        open_scenario_properties();
        smoke_require(findChild<QLabel *>("scenarioExtrapolation")
                          ->text()
                          .contains(QStringLiteral("已使用 EOP 外推")),
                      "Scenario does not identify actual extrapolation");
        scenario_properties_->close();
        smoke_record("finite-eop-extrapolation-prepares-orbit-and-displays-provenance");
        save_project_to(smoke_dir_ + "/extrapolated.orbelle");
        smoke_cache_ = orbit_;
        smoke_data_lock_ = scenario_->data_lock();
        const auto index = eop_catalog_->preferred_index();
        eop_catalog_->set_extrapolation_days(index, 0);
        smoke_answer_data(false, "dataCoverageDialog");
        smoke_require(!start_project(project_snapshot(project_path_), project_path_) &&
                          orbit_ == smoke_cache_,
                      "Disabled extrapolation reused a stored permission");
        eop_catalog_->set_extrapolation_days(index, 3);
        smoke_answer_data(false);
        smoke_require(!start_project(project_snapshot(project_path_), project_path_) &&
                          scenario_->data_lock() == smoke_data_lock_,
                      "Declining a changed extrapolation policy changed data");
        smoke_record("extrapolation-policy-change-and-disable-recheck-on-open");
        eop_catalog_->set_extrapolation_days(index, 2);
        start_project(
            application::load_project(std::filesystem::path(project_path_.toStdWString())),
            project_path_);
        ++orbit_smoke_stage_;
        break;
    }
    case 92:
        smoke_require(last_project_outcome_ == "opened" && !project_dirty_ &&
                          scenario_->data_lock() == smoke_data_lock_ &&
                          eop_->data_info().extrapolation_days == 2,
                      "Extrapolation provenance did not survive save/reopen");
        smoke_record("extrapolation-project-roundtrip-keeps-original-coverage-and-policy");
        ++orbit_smoke_stage_;
        break;
    case 93:
        // Closing a property page can queue a native-window resize. Let Qt/VSG
        // settle before comparing swapchain captures to current widget sizes.
        if (++smoke_final_frames_ >= 4) {
            save_workbench_image("extrapolated-scene.png");
            ++orbit_smoke_stage_;
        }
        break;
    case 94:
        start_project(application::load_project(std::filesystem::path(
                          (smoke_dir_ + "/imported-data.orbelle").toStdWString())),
                      smoke_dir_ + "/imported-data.orbelle");
        ++orbit_smoke_stage_;
        break;
    default:
        throw std::logic_error("Stage does not belong to data_catalog");
    }
}
