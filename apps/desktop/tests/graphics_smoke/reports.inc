void Workbench::smoke_reports() {
    auto input = orbit_ ? orbit_->input : OrbitInput{};
    switch (orbit_smoke_stage_) {
    case 121:
        report_action_->trigger();
        smoke_require(report_dialog_ && workspace_->contains(report_dialog_),
                      "Report did not open in the workspace");
        smoke_choose_report("builtin:2");
        report_dialog_->findChild<QComboBox *>("reportFrame")->setCurrentIndex(1);
        report_dialog_->findChild<QDoubleSpinBox *>("reportStep")->setValue(600);
        QTimer::singleShot(0, [] {
            auto *naming = qobject_cast<QInputDialog *>(QApplication::activeModalWidget());
            if (!naming)
                throw std::runtime_error("Report template naming did not open");
            naming->setTextValue(QStringLiteral("轨道位置报告"));
            naming->accept();
        });
        report_dialog_->findChild<QAction *>("saveReport")->trigger();
        smoke_require(reports_.size() == 1 && project_dirty_,
                      "Report configuration was not attached to the project");
        smoke_final_frames_ = 0;
        ++orbit_smoke_stage_;
        break;
    case 122:
        if (++smoke_final_frames_ < 4)
            break;
        smoke_require(
            workspace_->rect().contains(
                report_dialog_->findChild<QPushButton *>("generateReport")
                    ->mapTo(workspace_, report_dialog_->findChild<QPushButton *>("generateReport")
                                            ->rect()
                                            .bottomRight())),
            "Report generation action is clipped by the workspace");
        save_workbench_image("report-configuration.png");
        report_dialog_->findChild<QPushButton *>("generateReport")->click();
        ++orbit_smoke_stage_;
        break;
    case 123: {
        if (!report_dialog_->findChild<QPushButton *>("generateReport")->isEnabled())
            break;
        const auto *table = report_dialog_->findChild<QTableView *>("reportTable");
        smoke_require(table->model()->rowCount() > 0, "Report generation did not produce a table");
        save_workbench_image("report-results.png");
        const auto row_count = table->model()->rowCount();
        QTimer::singleShot(0, this, [this] {
            auto *chooser =
                dynamic_cast<ReportColumnSelectionDialog *>(QApplication::activeModalWidget());
            smoke_require(chooser != nullptr, "Report column selection did not open");
            chooser->resize(340, 500);
            QCoreApplication::processEvents();
            auto *list = chooser->findChild<QTreeWidget *>("reportColumnList");
            smoke_require(chooser->width() == 340 && list->horizontalScrollBar()->maximum() == 0,
                          "Report column selection exceeds a narrow window");
            smoke_require(chooser->grab().save(smoke_dir_ + "/report-column-selection.png"),
                          "Cannot save report column selection capture");
            for (int i = 0; i < list->topLevelItemCount(); ++i) {
                auto *item = list->topLevelItem(i);
                const auto id = item->data(0, Qt::UserRole).toString();
                item->setCheckState(0, id == "state.x" || id == "state.y" ? Qt::Checked
                                                                          : Qt::Unchecked);
            }
            chooser->accept();
        });
        report_dialog_->findChild<QAction *>("reportChooseColumns")->trigger();
        smoke_require(table->model()->columnCount() == 2 &&
                          table->model()->rowCount() == row_count &&
                          report_dialog_->findChild<QTabBar *>("reportViews")->isTabEnabled(1),
                      "Hiding metadata columns lost rows or curves");
        save_workbench_image("report-selected-columns.png");
        QTimer::singleShot(0, this, [] {
            auto *chooser =
                dynamic_cast<ReportColumnSelectionDialog *>(QApplication::activeModalWidget());
            smoke_require(chooser != nullptr, "Report column selection did not reopen");
            chooser->findChild<QPushButton *>("reportColumnDefaults")->click();
            chooser->accept();
        });
        report_dialog_->findChild<QAction *>("reportChooseColumns")->trigger();
        smoke_record("report-columns-select-restore-and-preserve-curves");
        // Native desktop scaling can reduce the logical window height. Verify
        // usable data rows rather than an arbitrary total-height threshold.
        smoke_require(table->isVisible() && table->width() > 250 && table->rowHeight(0) > 0 &&
                          table->viewport()->height() >= 5 * table->rowHeight(0),
                      qPrintable(QString("Generated report is not visible in its workspace pane: "
                                         "visible=%1, table=%2x%3, panel=%4x%5")
                                     .arg(table->isVisible())
                                     .arg(table->width())
                                     .arg(table->height())
                                     .arg(report_dialog_->width())
                                     .arg(report_dialog_->height())));
        QApplication::setAttribute(Qt::AA_DontUseNativeDialogs, true);
        QTimer::singleShot(0, this, [this] {
            auto *chooser = qobject_cast<QFileDialog *>(QApplication::activeModalWidget());
            smoke_require(chooser != nullptr, "CSV export did not open a file chooser");
            chooser->selectFile(smoke_dir_ + "/report.csv");
            static_cast<QDialog *>(chooser)->accept();
        });
        report_dialog_->findChild<QPushButton *>("exportReportCsv")->click();
        QApplication::setAttribute(Qt::AA_DontUseNativeDialogs, false);
        ++orbit_smoke_stage_;
        break;
    }
    case 124: {
        if (!report_dialog_->findChild<QPushButton *>("generateReport")->isEnabled())
            break;
        QFile csv(smoke_dir_ + "/report.csv");
        smoke_require(csv.open(QIODevice::ReadOnly) && csv.readAll().contains("Time (UTC)"),
                      "Report CSV was not exported");
        set_selection(0);
        rename_selected_to(QStringLiteral("报告测试卫星"));
        smoke_require(!report_dialog_->findChild<QLabel *>("reportResultLabel")
                           ->text()
                           .contains(QStringLiteral("场景已变化")),
                      "Renaming incorrectly invalidated the calculated report");
        save_project_to(smoke_dir_ + "/reports.orbelle");
        const auto restored =
            application::load_project(std::filesystem::path(project_path_.toStdWString()));
        smoke_require(restored.reports.size() == 1 &&
                          restored.reports[0].query.step_nanoseconds == 600'000'000'000,
                      "Saved report configuration lost its sampling interval");
        start_project(restored, project_path_);
        smoke_record("reports-workspace-generate-csv-rename-and-project-save");
        ++orbit_smoke_stage_;
        break;
    }
    case 125:
        smoke_require(last_project_outcome_ == "opened" && reports_.size() == 1,
                      "Project reopen lost report configurations");
        report_action_->trigger();
        smoke_choose_report("template:" + QString::fromStdString(reports_.front().id));
        report_dialog_->findChild<QPushButton *>("generateReport")->click();
        ++orbit_smoke_stage_;
        break;
    case 126: {
        auto *generate = report_dialog_->findChild<QPushButton *>("generateReport");
        if (!generate->isEnabled())
            break;
        smoke_require(report_dialog_->findChild<QTableView *>("reportTable")->model()->rowCount() >
                          0,
                      "Reopened report could not regenerate");
        smoke_data_lock_ = scenario_->data_lock();
        for (const auto index : eop_catalog_->current_indices()) {
            const auto &info = eop_catalog_->providers()[index]->data_info();
            if (info.id == eop_->data_info().id && info.sha256 == eop_->data_info().sha256)
                eop_catalog_->set_extrapolation_days(
                    index, eop_->data_info().extrapolation_days == 30
                               ? 29
                               : eop_->data_info().extrapolation_days + 1);
        }
        smoke_answer_data(false);
        generate->click();
        smoke_require(generate->isEnabled() && scenario_->data_lock() == smoke_data_lock_,
                      "Declined report data update changed the active scenario");
        smoke_answer_data(true);
        generate->click();
        ++orbit_smoke_stage_;
        break;
    }
    case 127:
        if (!report_dialog_->findChild<QPushButton *>("generateReport")->isEnabled())
            break;
        smoke_require(last_project_outcome_ == "opened" &&
                          scenario_->data_lock() != smoke_data_lock_ && project_dirty_ &&
                          report_dialog_->findChild<QLabel *>("reportResultLabel")
                              ->text()
                              .contains(QStringLiteral("与当前场景一致")),
                      "Accepted report data update did not rebuild and resume generation");
        smoke_record("reports-reopen-and-confirm-current-data-before-regeneration");
        workspace_->close_page(report_dialog_);
        workspace_->select_view(0);
        fit_views();
        smoke_final_frames_ = 0;
        ++orbit_smoke_stage_;
        break;
    case 128:
        if (++smoke_final_frames_ >= 4)
            ++orbit_smoke_stage_;
        break;
    case 129: {
        auto *item = satellites_.at(1).item;
        objects_->scrollToItem(item);
        QTimer::singleShot(0, this, [this] {
            auto *menu = qobject_cast<QMenu *>(QApplication::activePopupWidget());
            smoke_require(menu != nullptr, "Object report context menu did not open");
            auto *action = menu->findChild<QAction *>("openObjectReport");
            smoke_require(action != nullptr, "Object report action is missing");
            // The clicked object's stable ID must survive unrelated selection changes.
            set_selection(0);
            action->trigger();
            menu->close();
        });
        objects_->customContextMenuRequested(objects_->visualItemRect(item).center());
        smoke_require(report_dialog_->findChild<QLabel *>("reportScopeLabel")->text() ==
                          item->text(0),
                      "Context report did not bind the right-clicked object");
        smoke_choose_report("builtin:1");
        QTimer::singleShot(0, [] {
            auto *naming = qobject_cast<QInputDialog *>(QApplication::activeModalWidget());
            if (!naming)
                throw std::runtime_error("Object template naming did not open");
            naming->setTextValue(QStringLiteral("对象报告模板"));
            naming->accept();
        });
        report_dialog_->findChild<QAction *>("saveReport")->trigger();
        smoke_require(reports_.back().query.object_ids ==
                          std::vector<std::string>{satellites_.at(1).orbit->input.object_id},
                      "Changing report type widened the object scope");
        report_dialog_->findChild<QPushButton *>("generateReport")->click();
        ++orbit_smoke_stage_;
        break;
    }
    case 130: {
        if (!report_dialog_->findChild<QPushButton *>("generateReport")->isEnabled())
            break;
        auto *model = report_dialog_->findChild<QTableView *>("reportTable")->model();
        smoke_require(model->rowCount() == 1 &&
                          model->data(model->index(0, smoke_report_column(model, "object.name")))
                                  .toString() == satellites_.at(1).name(),
                      "Object report included an unselected object");
        save_workbench_image("object-report-results.png");
        smoke_record("object-report-context-scope-survives-template-change");
        smoke_choose_report(
            QStringLiteral("builtin:%1")
                .arg(static_cast<int>(application::ReportKind::classical_elements)));
        report_dialog_->findChild<QPushButton *>("generateReport")->click();
        ++orbit_smoke_stage_;
        break;
    }
    case 131: {
        if (!report_dialog_->findChild<QPushButton *>("generateReport")->isEnabled())
            break;
        auto *views = report_dialog_->findChild<QTabBar *>("reportViews");
        smoke_require(views->isTabEnabled(1), "Classical elements did not expose a graph");
        views->setCurrentIndex(1);
        ++orbit_smoke_stage_;
        break;
    }
    case 132: {
        auto *plot = static_cast<ReportPlot *>(report_dialog_->findChild<QWidget *>("reportPlot"));
        smoke_require(plot && plot->isVisible() && plot->width() > 200 && plot->height() > 150,
                      "Report graph is not usable inside the workbench");
        auto *fields = report_dialog_->findChild<QPushButton *>("reportPlotFields");
        smoke_require(fields && fields->isEnabled() && plot->fields().size() == 1 &&
                          plot->fields().front().id == "elements.nu",
                      "Classical graph fields or default angle are incorrect");
        save_workbench_image("report-classical-curves.png");
        smoke_record("report-classical-elements-and-curves");
        workspace_->close_page(report_dialog_);
        workspace_->select_view(0);
        fit_views();
        smoke_final_frames_ = 0;
        ++orbit_smoke_stage_;
        break;
    }
    case 133:
        if (++smoke_final_frames_ >= 4)
            ++orbit_smoke_stage_;
        break;
    case 134: {
        set_selection(0);
        open_properties();
        auto *navigation = editor_->findChild<QTreeWidget *>("propertyNavigation");
        navigation->setCurrentItem(navigation->topLevelItem(0)->child(1));
        const auto orbit_before = satellites_.front().orbit;
        const auto geometry_before = satellites_.front().fixed_track->node();
        const auto other = satellites_.size() > 1 ? satellites_[1].display : DisplaySettings{};
        auto settings = satellites_.front().display;
        settings.track_2d = settings.track_3d = true;
        editor_->set_display(settings);
        editor_->findChild<QToolButton *>("trackColor2")->click();
        editor_->findChild<QComboBox *>("trackLineStyle")->setCurrentIndex(1);
        editor_->findChild<QDoubleSpinBox *>("trackLineWidth")->setValue(4);
        editor_->findChild<QPushButton *>("applyDisplay")->click();
        smoke_require(
            satellites_.front().display.line_color == 0xe9a2ba &&
                satellites_.front().display.line_style == application::LineStyle::dashed &&
                satellites_.front().display.line_width == 4 && project_dirty_ &&
                satellites_.front().orbit == orbit_before &&
                satellites_.front().fixed_track->node() == geometry_before &&
                (satellites_.size() < 2 || satellites_[1].display == other),
            "Track appearance rebuilt the orbit, lost settings or changed another object");
        save_workbench_image("satellite-style-editor.png");
        workspace_->close_page(properties_dialog_);
        workspace_->select_view(0);
        smoke_final_frames_ = 0;
        smoke_record("satellite-style-applied-without-recomputing-orbit");
        ++orbit_smoke_stage_;
        break;
    }
    case 135:
        if (++smoke_final_frames_ < 4 || !ground_map_->tracks_ready())
            break;
        save_workbench_image("satellite-style-globe.png");
        ground_map_->grab().save(smoke_dir_ + "/satellite-style-map.png");
        save_project_to(smoke_dir_ + "/satellite-styles.orbelle");
        start_project(
            application::load_project(std::filesystem::path(project_path_.toStdWString())),
            project_path_);
        ++orbit_smoke_stage_;
        break;
    case 136:
        smoke_require(last_project_outcome_ == "opened" &&
                          satellites_.front().display.line_color == 0xe9a2ba &&
                          satellites_.front().display.line_style ==
                              application::LineStyle::dashed &&
                          satellites_.front().display.line_width == 4,
                      "Project reopen lost satellite track appearance");
        smoke_record("satellite-style-survives-project-reopen");
        fit_views();
        smoke_final_frames_ = 0;
        ++orbit_smoke_stage_;
        break;
    case 137:
        if (++smoke_final_frames_ >= 4)
            ++orbit_smoke_stage_;
        break;
    default:
        throw std::logic_error("Stage does not belong to reports");
    }
}
