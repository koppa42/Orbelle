void Workbench::smoke_sensors_navigation() {
    auto input = orbit_ ? orbit_->input : OrbitInput{};
    switch (orbit_smoke_stage_) {
    case 138: {
        auto p = project_snapshot(project_path_);
        smoke_require(!p.satellites.empty(), "Access smoke requires a satellite");
        if (p.ground_stations.empty())
            p.ground_stations.push_back({"access-station", "测控站", {}});
        const std::array epochs{p.first};
        const auto state = core::transform_state(
            runtime::make_orbit_session(p.satellites[0].orbit, 1, active_time().get())
                .sample(epochs)
                .states()[0],
            core::FrameId::itrf, *frames_, eop_->at(p.first));
        const auto location = runtime::itrf_to_wgs84(state);
        p.ground_stations[0].location = {location.longitude_rad, location.latitude_rad, 0};
        p.ground_stations[0].minimum_elevation_rad = 5 * std::numbers::pi / 180;
        start_project(std::move(p), project_path_, true, 0);
        ++orbit_smoke_stage_;
        break;
    }
    case 139: {
        if (project_candidate_.valid())
            break;
        if (!sensor_editor_ || workspace_->current_document() != sensor_editor_) {
            set_selection(0);
            edit_sensor(true);
            sensor_editor_->findChild<QLineEdit *>("sensorName")
                ->setText(QStringLiteral("对地观测仪"));
            sensor_editor_->findChild<QuantityEditor *>("sensorHalfAngle")
                ->set_value(20, Unit::degree);
            // Embedding, responsive margins and the vertical scrollbar each queue
            // layout work. Allow the same bounded settling period as other pane checks.
            smoke_wait_frame_ = frame_count_ + 4;
            break;
        }
        if (frame_count_ < smoke_wait_frame_)
            break;
        auto *scroll = sensor_editor_->findChild<QScrollArea *>("sensorScroll");
        auto *submit = sensor_editor_->findChild<QPushButton *>("applySensor");
        smoke_require(scroll && submit && scroll->horizontalScrollBar()->maximum() == 0,
                      "Embedded sensor editor requires horizontal scrolling");
        smoke_require(sensor_editor_->rect().contains(
                          QRect(submit->mapTo(sensor_editor_, QPoint{}), submit->size())),
                      "Sensor action is clipped in the workspace");
        save_workbench_image("sensor-editor.png");
        const auto scroll_details =
            "Sensor horizontal scrollbar is visible after layout: maximum=" +
            std::to_string(scroll->horizontalScrollBar()->maximum()) +
            ", viewport=" + std::to_string(scroll->viewport()->width()) +
            ", contents=" + std::to_string(scroll->widget()->width());
        smoke_require(!scroll->horizontalScrollBar()->isVisible(), scroll_details.c_str());
        smoke_require(scroll->widget()->width() <= scroll->viewport()->width(),
                      qPrintable(QString("Sensor form is wider than its workspace viewport: "
                                         "contents=%1, viewport=%2, minimum=%3, hint=%4")
                                     .arg(scroll->widget()->width())
                                     .arg(scroll->viewport()->width())
                                     .arg(scroll->widget()->minimumWidth())
                                     .arg(scroll->widget()->minimumSizeHint().width())));
        smoke_record("sensor-editor-embedded-layout");
        submit->click();
        ++orbit_smoke_stage_;
        break;
    }
    case 140: {
        if (project_candidate_.valid())
            break;
        smoke_require(sensors_.size() == 1 && satellites_[0].item->childCount() == 1,
                      "Sensor was not attached to its satellite");
        save_workbench_image("sensor-inserted.png");
        smoke_require(satellites_[0].item->isExpanded() &&
                          !objects_->visualItemRect(satellites_[0].item->child(0)).isEmpty(),
                      "Inserted sensor is hidden under a collapsed satellite");
        smoke_require(selected_sensor() && selected_sensor()->object_id == sensors_[0].object_id &&
                          objects_->currentItem() == satellites_[0].item->child(0),
                      "Inserting a sensor did not select the new child");
        smoke_record("sensor-insert-reveals-and-selects-child");
        open_properties();
        smoke_require(selected_sensor() && workspace_->current_document() == sensor_editor_,
                      "Sensor properties opened the parent");
        sensor_editor_->findChild<QLineEdit *>("sensorName")->setText("Discarded draft");
        sensor_editor_->close();
        smoke_require(sensors_[0].name() != "Discarded draft",
                      "Closing sensor editor applied a draft");
        smoke_record("sensor-child-properties-and-cancel");
        show_reports();
        report_dialog_->select_object(sensors_[0].object_id,
                                      application::ReportKind::access_intervals);
        report_dialog_->findChild<QCheckBox *>("reportFollowScenario")->setChecked(false);
        static_cast<EpochEditor *>(report_dialog_->findChild<QWidget *>("reportFirst"))
            ->set_epoch(scenario_->first());
        static_cast<EpochEditor *>(report_dialog_->findChild<QWidget *>("reportLast"))
            ->set_epoch(core::Epoch{scenario_->first().tai_nanoseconds() + 600'000'000'000});
        report_dialog_->findChild<QPushButton *>("generateReport")->click();
        ++orbit_smoke_stage_;
        break;
    }
    case 141: {
        if (!report_dialog_->findChild<QPushButton *>("generateReport")->isEnabled())
            break;
        auto *table = report_dialog_->findChild<QTableView *>("reportTable");
        smoke_require(table->model()->rowCount() > 0 && access_session_.has_value(),
                      "Access report produced no overhead pass");
        const auto index =
            table->model()->index(0, smoke_report_column(table->model(), "time.epoch"));
        table->doubleClicked(index);
        smoke_record("access-windows-timeline-and-seek");
        save_workbench_image("access-windows.png");
        smoke_choose_report(
            QStringLiteral("builtin:%1").arg(static_cast<int>(application::ReportKind::aer)));
        report_dialog_->findChild<QDoubleSpinBox *>("reportStep")->setValue(10);
        report_dialog_->findChild<QCheckBox *>("reportFollowScenario")->setChecked(false);
        static_cast<EpochEditor *>(report_dialog_->findChild<QWidget *>("reportFirst"))
            ->set_epoch(scenario_->first());
        static_cast<EpochEditor *>(report_dialog_->findChild<QWidget *>("reportLast"))
            ->set_epoch(core::Epoch{scenario_->first().tai_nanoseconds() + 600'000'000'000});
        report_dialog_->findChild<QPushButton *>("generateReport")->click();
        ++orbit_smoke_stage_;
        break;
    }
    case 142:
        if (!report_dialog_->findChild<QPushButton *>("generateReport")->isEnabled())
            break;
        smoke_require(snapshot_.epoch == scenario_->first() &&
                          ground_map_->epoch() == scenario_->first(),
                      "Access result seek did not reach both scene and map");
        smoke_require(report_dialog_->findChild<QTableView *>("reportTable")->model()->rowCount() >
                          1,
                      "AER report was not generated");
        report_dialog_->findChild<QTabBar *>("reportViews")->setCurrentIndex(1);
        smoke_record("aer-sensor-curve");
        save_workbench_image("access-aer.png");
        save_project_to(smoke_dir_ + "/access-sensors.orbelle");
        start_project(application::load_project(std::filesystem::path(
                          (smoke_dir_ + "/access-sensors.orbelle").toStdWString())),
                      smoke_dir_ + "/access-sensors.orbelle");
        ++orbit_smoke_stage_;
        break;
    case 143: {
        if (project_candidate_.valid())
            break;
        smoke_require(
            sensors_.size() == 1 && satellites_[0].item->childCount() == 1 &&
                std::abs(sensors_[0].pattern.half_angle_rad - 20 * std::numbers::pi / 180) < 1e-12,
            "Reopening lost sensor configuration");
        smoke_require(satellites_[0].item->isExpanded() &&
                          !objects_->visualItemRect(satellites_[0].item->child(0)).isEmpty(),
                      "Reopening hid the saved sensor");
        smoke_record("sensor-project-reopen");
        findChild<QLineEdit *>("objectSearch")->setText(QStringLiteral("观测仪"));
        objects_->setCurrentItem(satellites_[0].item->child(0));
        open_properties();
        const auto sensor_revision = scenario_->revision();
        sensor_editor_->findChild<QuantityEditor *>("sensorHalfAngle")->set_unit(Unit::radian);
        sensor_editor_->findChild<QPushButton *>("applySensor")->click();
        smoke_require(!busy() && scenario_->revision() == sensor_revision && !project_dirty_,
                      "Unchanged sensor Apply started work or dirtied the project");
        smoke_record("unchanged-sensor-apply-preserves-scene");
        sensor_editor_->findChild<QuantityEditor *>("sensorHalfAngle")->set_value(25, Unit::degree);
        sensor_editor_->findChild<QPushButton *>("applySensor")->click();
        ++orbit_smoke_stage_;
        break;
    }
    case 144:
        smoke_require(selected_sensor() && selected_sensor()->object_id == sensors_[0].object_id &&
                          std::abs(selected_sensor()->pattern.half_angle_rad -
                                   25 * std::numbers::pi / 180) < 1e-12 &&
                          !satellites_[0].item->isHidden() && satellites_[0].item->isExpanded() &&
                          satellites_[1].item->isHidden() && stations_[0].item->isHidden(),
                      "Sensor edit lost the child selection or active search filter");
        smoke_record("sensor-edit-preserves-selection-and-search");
        rename_selected_to(QStringLiteral("对地观测仪重命名"));
        ++orbit_smoke_stage_;
        break;
    case 145:
        smoke_require(selected_sensor() && selected_sensor()->object_id == sensors_[0].object_id &&
                          selected_name() == QStringLiteral("对地观测仪重命名") &&
                          objects_->currentItem()->text(0) == selected_name(),
                      "Sensor rename did not refresh the selected child");
        smoke_record("sensor-rename-refreshes-selected-child");
        {
            const auto orbit = satellites_[0].orbit;
            const auto sensor = *selected_sensor();
            const auto hash = runtime::report_input_sha256(project_snapshot(project_path_));
            open_properties();
            sensor_editor_->findChild<QLineEdit *>("sensorName")
                ->setText(QStringLiteral("属性页传感器"));
            sensor_editor_->findChild<QPushButton *>("applySensor")->click();
            smoke_require(!busy() && satellites_[0].orbit == orbit &&
                              selected_sensor()->object_id == sensor.object_id &&
                              selected_sensor()->parent_satellite_id ==
                                  sensor.parent_satellite_id &&
                              selected_sensor()->target_station_id == sensor.target_station_id &&
                              selected_name() == QStringLiteral("属性页传感器") &&
                              !sensor_editor_->has_unapplied_changes() &&
                              runtime::report_input_sha256(project_snapshot(project_path_)) == hash,
                          "Name-only sensor Apply rebuilt the scene or changed references");
            rename_selected_to(QStringLiteral("对地观测仪重命名"));
            smoke_record("sensor-name-apply-preserves-caches-and-references");
        }
        duplicate_selected();
        ++orbit_smoke_stage_;
        break;
    case 146:
        smoke_require(sensors_.size() == 2 && satellites_[0].item->childCount() == 2 &&
                          selected_sensor() &&
                          selected_sensor()->object_id == sensors_[1].object_id &&
                          selected_sensor()->object_id != sensors_[0].object_id &&
                          !objects_->visualItemRect(objects_->currentItem()).isEmpty() &&
                          satellites_[1].item->isHidden() && stations_[0].item->isHidden(),
                      "Sensor copy was not revealed and selected under the active filter");
        smoke_record("sensor-copy-reveals-new-child-under-search");
        findChild<QLineEdit *>("objectSearch")->clear();
        satellites_[0].item->setExpanded(false);
        set_selection(satellites_.size());
        rename_selected_to(QStringLiteral("测控站 · 更新"));
        ++orbit_smoke_stage_;
        break;
    case 147:
        smoke_require(!satellites_[0].item->isExpanded() && selected_station_ == 0 &&
                          stations_[0].name() == QStringLiteral("测控站 · 更新"),
                      "Updating a station reset another object's collapsed state");
        {
            const auto orbit = satellites_[0].orbit;
            const auto id = stations_[0].config.object_id;
            const auto hash = runtime::report_input_sha256(project_snapshot(project_path_));
            open_properties();
            station_editor_->findChild<QLineEdit *>("stationName")
                ->setText(QStringLiteral("属性页地面站"));
            station_editor_->findChild<QPushButton *>("applyGroundStation")->click();
            smoke_require(!busy() && satellites_[0].orbit == orbit &&
                              stations_[0].config.object_id == id &&
                              stations_[0].name() == QStringLiteral("属性页地面站") &&
                              !station_editor_->has_unapplied_changes() &&
                              runtime::report_input_sha256(project_snapshot(project_path_)) == hash,
                          "Name-only station Apply rebuilt the scene or changed identity");
            rename_selected_to(QStringLiteral("测控站 · 更新"));
            smoke_record("station-name-apply-preserves-caches-and-references");
        }
        findChild<QLineEdit *>("objectSearch")->setText(QStringLiteral("副本"));
        smoke_require(satellites_[0].item->isExpanded() && !satellites_[0].item->isHidden() &&
                          satellites_[0].item->child(0)->isHidden() &&
                          !satellites_[0].item->child(1)->isHidden() &&
                          stations_[0].item->isHidden(),
                      "Sensor search did not reveal only the matching child and its parent");
        objects_->setCurrentItem(satellites_[0].item->child(1));
        smoke_confirm_delete(false);
        smoke_require(sensors_.size() == 2 && selected_sensor() && !busy(),
                      "Cancelling sensor deletion changed the tree");
        smoke_confirm_delete(true);
        ++orbit_smoke_stage_;
        break;
    case 148:
        smoke_require(sensors_.size() == 1 && satellites_[0].item->childCount() == 1 &&
                          !selected_sensor() && selected_ == 0 && satellites_[0].item->isHidden(),
                      "Sensor deletion left a stale child, selection or search result");
        smoke_record("sensor-delete-refreshes-tree-and-search");
        findChild<QLineEdit *>("objectSearch")->clear();
        satellites_[0].item->setExpanded(false);
        scenario_item_->setExpanded(false);
        edit_sensor(true);
        sensor_editor_->findChild<QLineEdit *>("sensorName")->setText(QStringLiteral("后续传感器"));
        sensor_editor_->findChild<QPushButton *>("applySensor")->click();
        ++orbit_smoke_stage_;
        break;
    case 149:
        smoke_require(sensors_.size() == 2 && selected_sensor() &&
                          selected_name() == QStringLiteral("后续传感器") &&
                          scenario_item_->isExpanded() && satellites_[0].item->isExpanded() &&
                          !objects_->visualItemRect(objects_->currentItem()).isEmpty(),
                      "Inserting another sensor did not reveal it through collapsed ancestors");
        smoke_record("sensor-insert-expands-collapsed-ancestors");
        save_workbench_image("sensor-tree-refreshed.png");
        smoke_final_frames_ = 0;
        ++orbit_smoke_stage_;
        break;
    case 150:
        if (++smoke_final_frames_ < 4)
            return;
        smoke_responsive_size_ = size();
        sensor_editor_->close();
        slots_[1]->dock->hide();
        smoke_set_view_layout(0);
        set_selection(0);
        open_properties();
        editor_->findChild<QComboBox *>("propertyPageSelector")->setCurrentIndex(0);
        editor_->findChild<QuantityEditor *>("displayStep")->set_value(17, Unit::second);
        smoke_cache_ = orbit_;
        workspace_->split_page(properties_dialog_, view_panel_, Qt::RightEdge);
        resize(880, 700);
        smoke_final_frames_ = 0;
        smoke_wait_frame_ = frame_count_ + 4;
        ++orbit_smoke_stage_;
        break;
    case 151: {
        if (frame_count_ < smoke_wait_frame_ || !view_captures_ready())
            return;
        if (++smoke_final_frames_ == 1) {
            // Responsive forms have now recomputed their compact minimums; retry the
            // requested window size after that layout pass, still requiring 880 below.
            resize(880, 700);
            smoke_wait_frame_ = frame_count_ + 3;
            return;
        }
        save_workbench_image("responsive-workbench.png");
        // Compact navigation is page tabs at inspector widths, a selector when narrower.
        const bool compact_navigation =
            editor_->findChild<QComboBox *>("propertyPageSelector")->isVisible() ||
            editor_->findChild<QTabBar *>("propertyPageTabs")->isVisible();
        smoke_require(
            width() <= 880 && compact_navigation &&
                commands_->toolButtonStyle() == Qt::ToolButtonTextBesideIcon &&
                commands_->widgetForAction(eop_action_)->isVisible() &&
                commands_->rect().contains(commands_->actionGeometry(eop_action_)) &&
                findChild<QWidget *>("timelineControls")->height() > 50 &&
                smoke_action_in_workspace("applyOrbit"),
            qPrintable(
                QString("Narrow workbench: width=%1, propertyWidth=%2, compactNavigation=%3, "
                        "iconStyle=%4, controlsHeight=%5, applyVisible=%6, minimumWidth=%7, "
                        "minimumHint=%8")
                    .arg(width())
                    .arg(editor_->width())
                    .arg(compact_navigation)
                    .arg(commands_->toolButtonStyle())
                    .arg(findChild<QWidget *>("timelineControls")->height())
                    .arg(smoke_action_in_workspace("applyOrbit"))
                    .arg(minimumWidth())
                    .arg(minimumSizeHint().width())));
        smoke_require(orbit_ == smoke_cache_, "Resizing the workspace recalculated the orbit");
        smoke_record("responsive-workbench-and-native-view-resize");
        workspace_->detach_page(properties_dialog_);
        properties_dialog_->window()->resize(360, 520);
        ++orbit_smoke_stage_;
        break;
    }
    case 152:
        smoke_require(
            properties_dialog_->window()->width() == 360 &&
                editor_->findChild<QComboBox *>("propertyPageSelector")->isVisible() &&
                editor_->findChild<QuantityEditor *>("displayStep")->value(Unit::second) == 17,
            "Narrow floating properties lost the draft or retained a fixed width");
        smoke_require(properties_dialog_->window()->grab().save(
                          smoke_dir_ + "/responsive-floating-properties.png"),
                      "Cannot capture narrow floating properties");
        smoke_record("responsive-floating-properties-preserve-draft");
        workspace_->dock_page(properties_dialog_);
        resize(smoke_responsive_size_);
        slots_[1]->dock->show();
        smoke_wait_frame_ = frame_count_ + 4;
        ++orbit_smoke_stage_;
        break;
    case 153:
        if (frame_count_ < smoke_wait_frame_ || !view_captures_ready())
            return;
        smoke_require(
            orbit_ == smoke_cache_ &&
                editor_->findChild<QuantityEditor *>("displayStep")->value(Unit::second) == 17,
            "Restoring the workspace discarded the draft or recalculated the orbit");
        smoke_record("responsive-workbench-restore-preserves-state");
        editor_->findChild<QuantityEditor *>("displayStep")->set_value(60, Unit::second);
        properties_dialog_->close();
        smoke_set_view_layout(0);
        smoke_final_frames_ = 0;
        ++orbit_smoke_stage_;
        break;
    case 154: {
        if (++smoke_final_frames_ < 4)
            return;
        auto &slot = *slots_[0];
        auto *look = dynamic_cast<vsg::LookAt *>(slot.camera->viewMatrix.get());
        const auto eye = look->eye;
        const auto selected = selected_marker_index();
        const auto center = QPointF(slot.window->width() / 2.0, slot.window->height() / 2.0);
        const auto end = center + QPointF(60, 30);
        QMouseEvent press(QEvent::MouseButtonPress, center, center, Qt::LeftButton, Qt::LeftButton,
                          Qt::NoModifier);
        QMouseEvent move(QEvent::MouseMove, end, end, Qt::NoButton, Qt::LeftButton, Qt::NoModifier);
        QMouseEvent release(QEvent::MouseButtonRelease, end, end, Qt::LeftButton, Qt::NoButton,
                            Qt::NoModifier);
        QCoreApplication::sendEvent(slot.window, &press);
        QCoreApplication::sendEvent(slot.window, &move);
        QCoreApplication::sendEvent(slot.window, &release);
        smoke_require(vsg::length(look->eye - eye) > 1000 && selected_marker_index() == selected,
                      "3D rotation did not respond or changed object selection");
        smoke_camera_ = {look->eye, look->center, look->up};
        smoke_wait_frame_ = frame_count_ + 4;
        smoke_record("3d-drag-rotates-without-selecting");
        ++orbit_smoke_stage_;
        break;
    }
    case 155: {
        if (frame_count_ < smoke_wait_frame_)
            return;
        auto &slot = *slots_[0];
        auto *look = dynamic_cast<vsg::LookAt *>(slot.camera->viewMatrix.get());
        smoke_require(look->eye == smoke_camera_[0] && look->center == smoke_camera_[1] &&
                          look->up == smoke_camera_[2],
                      "3D camera drifted after mouse release");
        const auto center = QPointF(slot.window->width() / 2.0, slot.window->height() / 2.0);
        const auto end = center + QPointF(35, 20);
        QMouseEvent press(QEvent::MouseButtonPress, center, center, Qt::MiddleButton,
                          Qt::MiddleButton, Qt::NoModifier);
        QMouseEvent move(QEvent::MouseMove, end, end, Qt::NoButton, Qt::MiddleButton,
                         Qt::NoModifier);
        QCoreApplication::sendEvent(slot.window, &press);
        QCoreApplication::sendEvent(slot.window, &move);
        smoke_require(look->center != smoke_camera_[1], "3D middle drag did not pan");
        QKeyEvent cancel(QEvent::KeyPress, Qt::Key_Escape, Qt::NoModifier);
        QCoreApplication::sendEvent(slot.window, &cancel);
        smoke_require(look->eye == smoke_camera_[0] && look->center == smoke_camera_[1],
                      "3D Escape did not restore the camera");
        const auto before = vsg::length(look->eye - look->center);
        QWheelEvent wheel(center, center, QPoint{0, 3}, QPoint{0, 120}, Qt::NoButton,
                          Qt::NoModifier, Qt::ScrollUpdate, false);
        QCoreApplication::sendEvent(slot.window, &wheel);
        const auto distance = vsg::length(look->eye - look->center);
        smoke_require(std::abs(distance / before - 1 / std::pow(1.2, 3.0 / 60)) < 1e-9,
                      "3D touchpad scrolling lost its continuous input magnitude");
        smoke_navigation_distance_ = distance / 1.3;
        zoom_view(0, 1 / 1.3);
        smoke_wait_frame_ = frame_count_ + 12;
        smoke_record("3d-release-stops-pan-cancels-and-touchpad-is-continuous");
        ++orbit_smoke_stage_;
        break;
    }
    case 156: {
        if (frame_count_ < smoke_wait_frame_)
            return;
        const auto *look = dynamic_cast<vsg::LookAt *>(slots_[0]->camera->viewMatrix.get());
        smoke_require(
            std::abs(vsg::length(look->eye - look->center) / smoke_navigation_distance_ - 1) < 1e-8,
            "3D toolbar zoom did not settle at its requested distance");
        smoke_require(orbit_ == smoke_cache_, "Camera navigation recalculated the orbit");
        QKeyEvent fit(QEvent::KeyPress, Qt::Key_Home, Qt::NoModifier);
        QCoreApplication::sendEvent(slots_[0]->window, &fit);
        smoke_require(vsg::length(look->center) < 1, "3D Home did not fit the orbital scene");
        smoke_record("3d-smooth-zoom-home-and-orbit-cache-preserved");
        smoke_final_frames_ = 0;
        ++orbit_smoke_stage_;
        break;
    }
    case 157:
        if (++smoke_final_frames_ >= 4)
            ++orbit_smoke_stage_;
        break;
    default:
        throw std::logic_error("Stage does not belong to sensors_navigation");
    }
}
