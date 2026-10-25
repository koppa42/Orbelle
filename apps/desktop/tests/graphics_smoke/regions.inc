void Workbench::smoke_regions() {
    switch (orbit_smoke_stage_) {
    case 216: {
        smoke_cache_ = orbit_;
        edit_region(true);
        region_editor_->findChild<QLineEdit *>("regionName")->setText("Coverage region");
        region_editor_->findChild<QuantityEditor *>("regionGridSpacing")
            ->set_value(20, Unit::degree);
        region_editor_->findChild<QComboBox *>("regionShape")->setCurrentIndex(1);
        auto *vertices = region_editor_->findChild<QTableWidget *>("regionVertices");
        vertices->setRowCount(3);
        const std::array<std::array<double, 2>, 3> coordinates{{{90, 10}, {130, 10}, {110, 50}}};
        for (int row = 0; row < 3; ++row)
            for (int col = 0; col < 2; ++col)
                static_cast<QuantityEditor *>(vertices->cellWidget(row, col))
                    ->set_value(coordinates[row][col], Unit::degree);
        auto draft = region_editor_->region();
        draft.display.color = 0x4080c0;
        region_editor_->set_region(draft, true);
        region_editor_->findChild<QPushButton *>("applyRegion")->click();
        smoke_require(regions_.size() == 1 && selected_region_ == 0 && orbit_ == smoke_cache_,
                      "Creating a region failed or rebuilt the satellite trajectory");
        smoke_final_frames_ = 0;
        ++orbit_smoke_stage_;
        break;
    }
    case 217:
        if (++smoke_final_frames_ < 4)
            break;
        save_workbench_image("region-properties.png");
        region_editor_->close();
        workspace_->select_view(1);
        ground_map_->reset_view();
        set_playing(true);
        smoke_final_frames_ = 0;
        ++orbit_smoke_stage_;
        break;
    case 218:
        ++smoke_final_frames_;
        if (smoke_final_frames_ == 4)
            ground_map_->zoom_centered(2);
        if (smoke_final_frames_ == 12)
            ground_map_->zoom_centered(.5);
        if (smoke_final_frames_ < 20)
            break;
        set_playing(false);
        smoke_require(orbit_ == smoke_cache_ && region_group_ &&
                          region_group_->children.size() == 1,
                      "Region playback lost static geometry or recomputed the orbit");
        save_workbench_image("region-2d.png");
        smoke_record("region-create-and-playback-without-orbit-rebuild");
        workspace_->select_view(0);
        smoke_require(focus_action_->isEnabled(), "Region focus action is disabled");
        focus_satellite();
        smoke_final_frames_ = 0;
        ++orbit_smoke_stage_;
        break;
    case 219: {
        if (++smoke_final_frames_ < 4)
            break;
        if (smoke_final_frames_ == 4) {
            save_workbench_image("region-3d.png");
            const auto cached = region_group_;
            set_selection({});
            set_selection(satellites_.size() + stations_.size());
            smoke_require(region_group_ == cached,
                          "Region selection rebuilt static surface geometry");
            edit_region(false);
            region_editor_->findChild<QComboBox *>("propertyPageSelector")->setCurrentIndex(3);
            smoke_require(region_editor_->findChild<QPushButton *>("regionColor")->text() ==
                              "#4080c0",
                          "Region display page lost the shared color");
            save_workbench_image("region-display-properties.png");
            region_editor_->findChild<QDoubleSpinBox *>("regionOpacity")->setValue(100);
            region_editor_->findChild<QPushButton *>("applyRegion")->click();
            region_editor_->close();
        } else if (smoke_final_frames_ == 8) {
            save_workbench_image("region-3d-opaque.png");
            region_editor_->findChild<QCheckBox *>("regionFill")->setChecked(false);
            region_editor_->findChild<QPushButton *>("applyRegion")->click();
            smoke_require(!regions_.front().display.fill,
                          "Disabling region fill did not reach the project");
        } else if (smoke_final_frames_ == 12) {
            save_workbench_image("region-3d-unfilled.png");
            region_editor_->findChild<QCheckBox *>("regionFill")->setChecked(true);
            region_editor_->findChild<QDoubleSpinBox *>("regionOpacity")->setValue(12);
            region_editor_->findChild<QPushButton *>("applyRegion")->click();
            smoke_require(orbit_ == smoke_cache_ && regions_.front().display.fill &&
                              regions_.front().display.opacity == .12,
                          "Region display edits rebuilt the orbit or lost fill settings");
        } else if (smoke_final_frames_ == 16) {
            smoke_record("region-shared-color-fill-opacity-and-cached-selection");
            ++orbit_smoke_stage_;
        }
        break;
    }
    case 220:
        QTimer::singleShot(0, this, [this] {
            smoke_require(report_creation_, "Region report chooser did not open");
            report_creation_->accept();
            auto *region =
                report_creation_->findChild<ObjectSelectionField *>("reportCreationObject0");
            auto *assets =
                report_creation_->findChild<ObjectSelectionField *>("reportCreationObject1");
            smoke_require(region && assets && region->id() == regions_.front().object_id,
                          "Coverage chooser lost the region context");
            assets->set_selection({{satellites_.front().orbit->input.object_id}, false});
            report_creation_->accept();
        });
        timer_.stop();
        create_report(regions_.front().object_id, application::ReportKind::coverage_grid);
        timer_.start();
        smoke_require(report_dialog_, "Region report did not open");
        smoke_require(!report_dialog_->findChild<QDoubleSpinBox *>("coverageResolution"),
                      "Report duplicated the region grid spacing editor");
        report_dialog_->findChild<QDoubleSpinBox *>("reportStep")->setValue(60);
        report_dialog_->findChild<QPushButton *>("generateReport")->click();
        ++orbit_smoke_stage_;
        break;
    case 221: {
        if (!report_dialog_->findChild<QPushButton *>("generateReport")->isEnabled())
            break;
        smoke_require(report_dialog_->findChild<QTableView *>("reportTable")->model()->rowCount() >
                          0,
                      "Named region coverage did not produce results");
        save_workbench_image("region-coverage-report.png");
        QTimer::singleShot(0, [] {
            auto *name = qobject_cast<QInputDialog *>(QApplication::activeModalWidget());
            if (!name)
                throw std::runtime_error("Coverage template naming did not open");
            name->setTextValue("Region coverage");
            name->accept();
        });
        report_dialog_->findChild<QAction *>("saveReport")->trigger();
        save_project_to(smoke_dir_ + "/region-coverage.orbelle");
        smoke_record("region-coverage-report-and-template-save");
        ++orbit_smoke_stage_;
        break;
    }
    case 222: {
        const auto path = smoke_dir_ + "/region-coverage.orbelle";
        start_project(application::load_project(std::filesystem::path(path.toStdWString())), path);
        smoke_final_frames_ = 0;
        ++orbit_smoke_stage_;
        break;
    }
    case 223:
        if (project_candidate_.valid() || ++smoke_final_frames_ < 4)
            break;
        smoke_require(
            regions_.size() == 1 && regions_.front().boundary.vertices.size() == 3 &&
                regions_.front().display.fill && regions_.front().display.opacity == .12 &&
                regions_.front().display.color == 0x4080c0 && region_items_.size() == 1 &&
                std::ranges::any_of(reports_,
                                    [this](const auto &r) {
                                        return r.query.region_id == regions_.front().object_id;
                                    }),
            "Reopened project lost region geometry or report binding");
        set_selection(satellites_.size() + stations_.size());
        open_properties();
        smoke_require(!region_editor_->has_unapplied_changes(),
                      "Reopened region has a phantom draft");
        region_editor_->close();
        workspace_->select_view(0);
        smoke_record("region-project-reopen-and-properties");
        ++orbit_smoke_stage_;
        break;
    default:
        throw std::logic_error("Stage does not belong to regions");
    }
}
