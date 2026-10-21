void Workbench::smoke_workspace_views() {
    auto input = orbit_ ? orbit_->input : OrbitInput{};
    switch (orbit_smoke_stage_) {
    case 26: {
        const auto time = runtime::make_builtin_time_provider();
        const auto today = QDateTime::currentDateTimeUtc().date();
        const auto tomorrow = today.addDays(1);
        activate_scenario(application::Scenario(
            "Today", time->from_utc({today.year(), today.month(), today.day(), 0, 0, 0, 0}),
            time->from_utc({tomorrow.year(), tomorrow.month(), tomorrow.day(), 0, 0, 0, 0})));
        begin_insert_satellite();
        editor_->submit();
        ++orbit_smoke_stage_;
        break;
    }
    case 27:
        smoke_require(last_orbit_outcome_ == "applied" && satellites_.size() == 1 &&
                          eop_ == eop_current_ && orbit_->input.step_seconds == 60 &&
                          orbit_->track.samples().states().size() < 200,
                      "Current UTC one-day scenario failed to propagate with current EOP data");
        smoke_record("current-utc-day-orbit-and-predicted-eop");
        smoke_require(!commands_->actions().contains(properties_action_),
                      "Properties action remains in the command bar");
        open_scenario_properties();
        smoke_require(workspace_->current_document() == scenario_properties_,
                      "Scenario properties did not open in the workspace");
        smoke_require(scenario_properties_->grab().save(smoke_dir_ + "/scenario-properties.png"),
                      "Cannot save scenario properties");
        scenario_properties_->close();
        smoke_record("scenario-tree-properties");
        open_properties();
        smoke_require(
            properties_dialog_->grab().save(smoke_dir_ + "/current-object-properties.png"),
            "Cannot save current object properties");
        properties_dialog_->close();
        slots_[1]->dock->hide();
        fit_views();
        orbit_capture_ = true;
        ++orbit_smoke_stage_;
        break;
    case 28:
        save_workbench_image("current-day-workbench.png");
        workspace_->split_page(map_panel_, view_panel_, Qt::RightEdge);
        ground_map_->reset_view();
        fit_views();
        ++orbit_smoke_stage_;
        break;
    case 29: {
        smoke_require(ground_map_->track_count() == satellites_.size() &&
                          ground_map_->epoch() == snapshot_.epoch,
                      "2D map does not share the published scenario and epoch");
        smoke_require(QLineF(ground_coordinates({7e6, 0, 0}), QPointF(0, 0)).length() < 1e-9 &&
                          QLineF(ground_coordinates({0, 7e6, 0}), QPointF(90, 0)).length() < 1e-9 &&
                          std::abs(ground_coordinates({0, 0, 7e6}).y() - 90) < 1e-9,
                      "2D geodetic axes are incorrect");
        const double e2 = 6.6943799901413165e-3;
        const double radius = 6378137.0 / std::sqrt(1 - e2 * 0.5);
        const auto latitude45 = ground_coordinates(
            {(radius + 500000) / std::sqrt(2.0), 0, (radius * (1 - e2) + 500000) / std::sqrt(2.0)});
        smoke_require(std::abs(latitude45.y() - 45) < 1e-6,
                      "2D latitude is geocentric instead of WGS84 geodetic");
        const auto seam = ground_path({QPointF(179, 10), QPointF(-179, 12)});
        smoke_require(seam.elementCount() == 4 && seam.elementAt(1).x == 180 &&
                          seam.elementAt(2).isMoveTo() && seam.elementAt(2).x == -180 &&
                          std::abs(seam.elementAt(1).y - 11) < 1e-12,
                      "Ground track draws a false segment across the antimeridian");
        const auto exact_seam = ground_path({QPointF(180, 10), QPointF(-180, 10)});
        for (int i = 0; i < exact_seam.elementCount(); ++i)
            smoke_require(std::isfinite(exact_seam.elementAt(i).x) &&
                              std::isfinite(exact_seam.elementAt(i).y),
                          "Exact antimeridian produced an invalid path");
        for (const int direction : {-1, 1}) {
            const auto vertical =
                ground_path({QPointF(direction * 180, 10), QPointF(-direction * 180, 12)});
            smoke_require(std::abs(vertical.length() - 2.0) < 1e-12,
                          "Ground track dropped latitude changes along the antimeridian");
        }
        save_workbench_image("workbench-2d-3d.png");
        smoke_require(ground_map_->grab().save(smoke_dir_ + "/ground-map.png"),
                      "Cannot save 2D map");
        set_selection(std::nullopt);
        const auto point = ground_map_->marker_position(0);
        const auto global = ground_map_->mapToGlobal(point);
        QMouseEvent press(QEvent::MouseButtonPress, point, global, Qt::LeftButton, Qt::LeftButton,
                          Qt::NoModifier);
        QMouseEvent release(QEvent::MouseButtonRelease, point, global, Qt::LeftButton, Qt::NoButton,
                            Qt::NoModifier);
        QCoreApplication::sendEvent(ground_map_, &press);
        QCoreApplication::sendEvent(ground_map_, &release);
        smoke_require(selected_ == 0 && objects_->currentItem() == satellites_[0].item,
                      "Picking a 2D satellite does not select the same scene object");
        smoke_record("2d-geodetic-projection-seam-and-selection");
        seek_epoch(scenario_->last());
        ++orbit_smoke_stage_;
        break;
    }
    case 30: {
        smoke_require(ground_map_->epoch() == scenario_->last() &&
                          ground_map_->epoch() == snapshot_.epoch &&
                          QLineF(ground_map_->marker_coordinates(0),
                                 ground_coordinates(display_to_itrf_->apply_position(
                                     snapshot_.markers[0].position_m)))
                                  .length() < 2e-5,
                      "2D/3D epoch or position diverged after a timeline seek");
        const auto center = QPointF(ground_map_->rect().center());
        QWheelEvent wheel(center, ground_map_->mapToGlobal(center), QPoint{}, QPoint{0, 120},
                          Qt::NoButton, Qt::NoModifier, Qt::NoScrollPhase, false);
        QCoreApplication::sendEvent(ground_map_, &wheel);
        smoke_require(ground_map_->zoom_factor() > 1, "2D wheel zoom did not respond");
        workspace_->detach_page(map_panel_);
        map_panel_->window()->resize(780, 480);
        smoke_record("2d-3d-shared-time-and-zoom");
        ++orbit_smoke_stage_;
        break;
    }
    case 31:
        smoke_require(workspace_->is_floating(map_panel_) && map_panel_->isVisible(),
                      "2D view cannot float");
        ground_map_->reset_view();
        smoke_require(map_panel_->window()->grab().save(smoke_dir_ + "/ground-map-floating.png"),
                      "Cannot save floating 2D view");
        workspace_->dock_page(map_panel_);
        workspace_->select_view(0);
        slots_[1]->dock->show();
        seek_epoch(scenario_->first());
        fit_views();
        smoke_record("2d-floating-and-dock-restore");
        ++orbit_smoke_stage_;
        break;
    case 32:
        smoke_require(ground_map_->epoch() == snapshot_.epoch,
                      "Hidden 2D view lost its current epoch");
        slots_[1]->dock->hide();
        seek_epoch(core::Epoch{scenario_->first().tai_nanoseconds() + 43'200'123'000'000});
        timeline_->zoom(4);
        fit_views();
        ++orbit_smoke_stage_;
        break;
    case 33: {
        if (!view_captures_ready())
            return;
        save_workbench_image("timeline-zoom.png");
        const auto epoch = snapshot_.epoch;
        const auto visible = timeline_->visible_last() - timeline_->visible_first();
        smoke_require(visible == 21'600'000, "Timeline zoom did not expose a six-hour interval");
        set_selection(std::nullopt);
        set_selection(0);
        smoke_require(timeline_->visible_last() - timeline_->visible_first() == visible &&
                          snapshot_.epoch == epoch,
                      "Changing selected object reset the visible interval or simulation time");
        smoke_record("timeline-zoom-and-selection-preserves-range");
        timeline_->zoom(1'000'000);
        const auto plot = timeline_->detail_rect();
        const auto point = QPointF(plot.left() + plot.width() * 0.371, plot.center().y());
        smoke_timeline_epoch_ = timeline_->visible_first() + 371;
        set_playing(true);
        QMouseEvent press(QEvent::MouseButtonPress, point, timeline_->mapToGlobal(point),
                          Qt::LeftButton, Qt::LeftButton, Qt::NoModifier);
        QMouseEvent release(QEvent::MouseButtonRelease, point, timeline_->mapToGlobal(point),
                            Qt::LeftButton, Qt::NoButton, Qt::NoModifier);
        QCoreApplication::sendEvent(timeline_, &press);
        QCoreApplication::sendEvent(timeline_, &release);
        smoke_require(!playing_, "Timeline scrubbing did not pause playback");
        ++orbit_smoke_stage_;
        break;
    }
    case 34:
        smoke_require(snapshot_.epoch.tai_nanoseconds() == smoke_timeline_epoch_ * 1'000'000 &&
                          timeline_->current_time() == smoke_timeline_epoch_ &&
                          ground_map_->epoch() == snapshot_.epoch,
                      "Zoomed timeline seek lost millisecond precision or 2D/3D synchronization");
        smoke_record("timeline-precise-pointer-seek-and-pause");
        findChild<QComboBox *>("timelineScale")->setCurrentIndex(2);
        ++orbit_smoke_stage_;
        break;
    case 35:
        smoke_require(snapshot_.epoch.tai_nanoseconds() == smoke_timeline_epoch_ * 1'000'000 &&
                          time_label_->text() == calendar_text(snapshot_.epoch, CalendarScale::tt),
                      "Changing the timeline time standard changed the physical instant");
        smoke_record("timeline-scale-preserves-instant");
        smoke_require(time_toolbar_->grab().save(smoke_dir_ + "/timeline-precision-tt.png"),
                      "Cannot save the precise TT time ruler");
        findChild<QComboBox *>("timelineScale")->setCurrentIndex(0);
        timeline_->reset_view();
        findChild<QAction *>("showTimelineObject")->setChecked(true);
        ++orbit_smoke_stage_;
        break;
    case 36:
        smoke_require(time_toolbar_->grab().save(smoke_dir_ + "/timeline-object-expanded.png"),
                      "Cannot save the optional object interval");
        findChild<QAction *>("showTimelineObject")->setChecked(false);
        smoke_timeline_epoch_ =
            scenario_->first().tai_nanoseconds() / 1'000'000 + 13 * 3'600'000 + 1234;
        timer_.stop();
        QTimer::singleShot(0, this, [this] {
            auto *dialog = findChild<QDialog *>("jumpTimeDialog");
            if (!dialog) {
                fail("Clicking the time readout did not open precise time input");
                return;
            }
            dialog->findChild<QLineEdit *>("epochValue")
                ->setText(calendar_text(core::Epoch{smoke_timeline_epoch_ * 1'000'000},
                                        CalendarScale::utc)
                              .replace("  ", " ") +
                          " UTC");
            dialog->findChild<QPushButton *>("applyOrbit")->click();
        });
        time_label_->click();
        timer_.start();
        ++orbit_smoke_stage_;
        break;
    case 37:
        if (!view_captures_ready())
            return;
        smoke_require(snapshot_.epoch.tai_nanoseconds() == smoke_timeline_epoch_ * 1'000'000 &&
                          ground_map_->epoch() == snapshot_.epoch,
                      "Precise readout jump did not update the shared epoch");
        smoke_require(timeline_->sizeHint().height() == 118,
                      "Optional satellite row did not collapse");
        smoke_record("timeline-readout-jump-and-optional-object-row");
        {
            auto *rate = findChild<QDoubleSpinBox *>("playbackRate");
            auto *slower = findChild<QAction *>("slowerPlayback");
            auto *faster = findChild<QAction *>("fasterPlayback");
            auto *presets = findChild<QToolButton *>("playbackRatePresets");
            const auto epoch = snapshot_.epoch;
            const auto first = timeline_->visible_first(), last = timeline_->visible_last();
            timer_.stop();
            QTimer::singleShot(0, presets->menu(), [this, presets] {
                smoke_require(presets->menu()->grab().save(smoke_dir_ + "/playback-rate-menu.png"),
                              "Cannot capture playback rate presets");
                for (auto *action : presets->menu()->actions())
                    if (action->data().toDouble() == 100)
                        action->trigger();
                presets->menu()->close();
            });
            presets->showMenu();
            timer_.start();
            smoke_require(rate->value() == 100 && playback_rate_ == 100 && !playing_,
                          "Selecting a playback preset did not update the effective rate");
            slower->trigger();
            smoke_require(playback_rate_ == 50, "Slower button did not select the preceding rate");
            faster->trigger();
            smoke_require(playback_rate_ == 100, "Faster button did not select the following rate");
            rate->findChild<QLineEdit *>()->setText(QStringLiteral("37.5 ×"));
            rate->interpretText();
            smoke_require(playback_rate_ == 37.5, "Custom playback rate was not accepted");
            slower->trigger();
            smoke_require(playback_rate_ == 20, "Custom rate did not step down to a preset");
            rate->setValue(37.5);
            faster->trigger();
            smoke_require(playback_rate_ == 50, "Custom rate did not step up to a preset");
            rate->setValue(0.1);
            smoke_require(!slower->isEnabled(), "Slower button is enabled at the minimum rate");
            rate->setValue(1000);
            smoke_require(!faster->isEnabled(), "Faster button is enabled at the maximum rate");
            rate->setValue(20);
            set_playing(true);
            faster->trigger();
            smoke_require(playing_ && playback_rate_ == 50,
                          "Changing the rate interrupted active playback");
            set_playing(false);
            rate->setValue(20);
            smoke_require(snapshot_.epoch == epoch && timeline_->visible_first() == first &&
                              timeline_->visible_last() == last,
                          "Changing the playback rate sought time or changed the visible interval");
            smoke_record("timeline-playback-rate-controls");
        }
        save_workbench_image("timeline-workbench.png");
        smoke_require(time_toolbar_->grab().save(smoke_dir_ + "/timeline.png"),
                      "Cannot save the compact timeline");
        // Restore the known visible marker fixture for the final native-window checks.
        slots_[1]->dock->show();
        seek_epoch(scenario_->first());
        fit_views();
        orbit_capture_ = true;
        ++orbit_smoke_stage_;
        break;
    case 38:
        // Qt applies dock layout changes on the next event-loop iteration.
        // Wait for matching native geometry and readback before final validation.
        if (!view_captures_ready())
            return;
        slots_[1]->dock->hide();
        seek_epoch(core::Epoch{scenario_->first().tai_nanoseconds() + 6 * 3600'000'000'000LL});
        smoke_set_view_layout(2);
        ++orbit_smoke_stage_;
        break;
    case 39:
        if (!view_captures_ready() || !smoke_caches_ready())
            return;
        smoke_require(ground_map_->isVisible() && slots_[0]->container->isVisible(),
                      "Parallel view layout did not expose both renderers");
        save_workbench_image("views-parallel.png");
        smoke_view_epoch_ = snapshot_.epoch;
        smoke_sun_ = sun_direction_;
        smoke_set_view_layout(1);
        ++orbit_smoke_stage_;
        break;
    case 40:
        smoke_require(ground_map_->isVisible() && !slots_[0]->container->isVisible() &&
                          snapshot_.epoch == smoke_view_epoch_ && selected_ == 0,
                      "2D-only layout lost its content, time or selection");
        save_workbench_image("views-2d.png");
        smoke_map_image_ = ground_map_->grab().toImage();
        lighting_action_->setChecked(false);
        smoke_require(ground_map_->grab().toImage() != smoke_map_image_,
                      "The 2D daylight setting did not affect the map");
        lighting_action_->setChecked(true);
        smoke_record("view-modes-and-shared-display-settings");
        seek_epoch(core::Epoch{scenario_->first().tai_nanoseconds() + 12 * 3600'000'000'000LL});
        ++orbit_smoke_stage_;
        break;
    case 41: {
        const auto dot = smoke_sun_[0] * sun_direction_[0] + smoke_sun_[1] * sun_direction_[1] +
                         smoke_sun_[2] * sun_direction_[2];
        smoke_require(ground_map_->epoch() == snapshot_.epoch && dot < 0.1 && dot > -0.1,
                      "2D-only seeking failed to rotate Earth-fixed sunlight over six hours");
        smoke_require(ground_map_->grab().toImage() != smoke_map_image_,
                      "2D night overlay did not follow the shared scene time");
        smoke_view_epoch_ = snapshot_.epoch;
        set_playing(true);
        ++orbit_smoke_stage_;
        break;
    }
    case 42:
        smoke_require(snapshot_.epoch != smoke_view_epoch_ &&
                          ground_map_->epoch() == snapshot_.epoch,
                      "Playback stopped when the 3D window was hidden");
        set_playing(false);
        smoke_record("2d-only-playback-and-solar-time-synchronization");
        smoke_set_view_layout(0);
        seek_epoch(core::Epoch{scenario_->first().tai_nanoseconds() + 6 * 3600'000'000'000LL});
        fit_views();
        ++orbit_smoke_stage_;
        break;
    case 43:
        if (!view_captures_ready() || !smoke_caches_ready())
            return;
        save_workbench_image("views-3d.png");
        smoke_globe_image_ = slots_[0]->captured_image;
        lighting_action_->setChecked(false);
        smoke_wait_frame_ = frame_count_ + 3;
        ++orbit_smoke_stage_;
        break;
    case 44:
        if (frame_count_ < smoke_wait_frame_ || !view_captures_ready())
            return;
        smoke_require(slots_[0]->captured_image != smoke_globe_image_,
                      "3D lighting switch did not change the rendered globe");
        smoke_record("3d-daylight-toggle-and-restored-native-view");
        lighting_action_->setChecked(true);
        smoke_set_view_layout(2);
        ++orbit_smoke_stage_;
        break;
    case 45:
        if (!view_captures_ready())
            return;
        smoke_require(ground_map_->isVisible() && slots_[0]->container->isVisible(),
                      "2D/3D pages did not return to split groups");
        smoke_record("map-tab-to-split-roundtrip");
        lighting_action_->setChecked(false);
        smoke_set_view_layout(0);
        slots_[1]->dock->show();
        seek_epoch(scenario_->first());
        fit_views();
        ++orbit_smoke_stage_;
        break;
    case 46:
        if (!view_captures_ready() || !smoke_caches_ready())
            return;
        slots_[1]->dock->hide();
        open_properties();
        smoke_require(workspace_->current_document() == properties_dialog_,
                      "Object properties did not open in the central workspace");
        smoke_require(editor_->findChild<QuantityEditor *>("displayStep")->value(Unit::second) ==
                          60,
                      "The default display step is not 60 seconds");
        editor_->findChild<QuantityEditor *>("displayStep")->set_value(45, Unit::second);
        workspace_->select_view(1);
        ++orbit_smoke_stage_;
        break;
    case 47:
        smoke_require(
            ground_map_->isVisible() && !workspace_->current_document() &&
                editor_->findChild<QuantityEditor *>("displayStep")->value(Unit::second) == 45,
            "Switching to a view tab discarded the property draft");
        workspace_->open(properties_dialog_, false);
        ++orbit_smoke_stage_;
        break;
    case 48: {
        auto *navigation = editor_->findChild<QTreeWidget *>("propertyNavigation");
        smoke_require(navigation && navigation->topLevelItemCount() == 3 &&
                          navigation->topLevelItem(0)->childCount() == 2,
                      "The property navigation does not group orbit and range under Basic");
        editor_->findChild<QuantityEditor *>("displayStep")->set_value(60, Unit::second);
        {
            auto *angle = editor_->findChild<QuantityEditor *>("raan");
            angle->set_value(180, Unit::degree);
            auto *selector = angle->findChild<QToolButton *>("quantityUnit");
            QTimer::singleShot(0, selector->menu(), [this, selector] {
                smoke_require(selector->menu()->grab().save(smoke_dir_ + "/quantity-unit-menu.png"),
                              "Cannot capture the quantity unit menu");
                for (auto *action : selector->menu()->actions())
                    if (action->data().toInt() == static_cast<int>(Unit::radian))
                        action->trigger();
                selector->menu()->close();
            });
            timer_.stop(); // Keep the scripted stage from re-entering the modal menu.
            selector->showMenu();
            timer_.start();
            smoke_require(
                angle->unit() == Unit::radian &&
                    std::abs(angle->value(Unit::degree) - 180) < 1e-12 &&
                    angle->findChild<QLineEdit *>("quantityValue")->text().startsWith("3.14159"),
                "Clickable angle unit did not convert the number");
            save_workbench_image("workspace-quantity-units.png");
            angle->set_unit(Unit::degree);
            // Unit changes are a draft; restore the actual orbit value before later checks.
            angle->set_value(runtime::to_keplerian(orbit_->input.initial,
                                                   core::GravitationalParameter{orbit_->input.mu()})
                                 .raan_rad,
                             Unit::radian);
        }
        save_workbench_image("workspace-properties.png");
        smoke_require(smoke_action_in_workspace("applyOrbit"),
                      "Orbit Apply button is clipped by the central workspace");
        navigation->topLevelItem(0)->setExpanded(false);
        smoke_require(!navigation->topLevelItem(0)->isExpanded(), "Property group cannot collapse");
        navigation->topLevelItem(0)->setExpanded(true);
        navigation->setCurrentItem(navigation->topLevelItem(0)->child(1));
        ++orbit_smoke_stage_;
        break;
    }
    case 49:
        save_workbench_image("workspace-display-settings.png");
        smoke_require(smoke_action_in_workspace("applyDisplay"),
                      "Display Apply button is clipped by the central workspace");
        workspace_->detach_page(properties_dialog_);
        ++orbit_smoke_stage_;
        break;
    case 50:
        smoke_require(
            workspace_->is_floating(properties_dialog_) &&
                editor_->findChild<QuantityEditor *>("displayStep")->value(Unit::second) == 60,
            "Detaching properties changed the field values");
        smoke_require(
            properties_dialog_->window()->grab().save(smoke_dir_ + "/properties-detached.png"),
            "Cannot capture detached properties");
        workspace_->dock_page(properties_dialog_);
        smoke_require(workspace_->current_document() == properties_dialog_,
                      "Detached properties did not return to the workspace");
        properties_dialog_->close();
        smoke_require(isVisible() && !workspace_->contains(properties_dialog_),
                      "Closing properties closed the workbench");
        smoke_record("grouped-properties-tabs-draft-and-detach-roundtrip");
        workspace_->select_view(0);
        smoke_cache_ = orbit_;
        smoke_view_epoch_ = snapshot_.epoch;
        smoke_require(display_frame_ == core::FrameId::gcrf &&
                          snapshot_.frame == core::FrameId::gcrf,
                      "3D did not default to GCRF");
        frame_choices_.front()->setCurrentIndex(1);
        ++orbit_smoke_stage_;
        break;
    default:
        throw std::logic_error("Stage does not belong to workspace_views");
    }
}
