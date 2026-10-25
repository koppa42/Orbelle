void Workbench::smoke_display_cache_docking() {
    switch (orbit_smoke_stage_) {
    case 51: {
        const auto direct = orbit_->session.sample(std::array{snapshot_.epoch}).states().front();
        const auto fixed = display_to_itrf_->apply_position(direct.position_m);
        const auto shown = snapshot_.markers.front().position_m;
        smoke_require(
            display_frame_ == core::FrameId::itrf && snapshot_.frame == core::FrameId::itrf &&
                snapshot_.epoch == smoke_view_epoch_ && orbit_ == smoke_cache_ &&
                std::hypot(fixed[0] - shown[0], fixed[1] - shown[1], fixed[2] - shown[2]) <= 1,
            "Switching the 3D frame changed time/cache or used the wrong coordinates");
        frame_choices_.front()->setCurrentIndex(0);
        const auto inertial = snapshot_.markers.front().position_m;
        smoke_require(std::hypot(direct.position_m[0] - inertial[0],
                                 direct.position_m[1] - inertial[1],
                                 direct.position_m[2] - inertial[2]) <= 1,
                      "Returning to GCRF did not restore the inertial state");
        smoke_record("gcrf-itrf-frame-switch-without-propagation");
        const core::Epoch jump{scenario_->first().tai_nanoseconds() + 18 * 3600'000'000'000LL};
        smoke_require(!cache_covers(*orbit_, jump, jump), "Seek test unexpectedly already cached");
        const auto immediate = sample_frame(jump).markers.front().position_m;
        const auto exact = orbit_->session.sample(std::array{jump}).states().front().position_m;
        smoke_require(immediate == exact,
                      "Uncached time did not query the analysis session directly");
        smoke_publications_ = display_cache_publications_;
        smoke_render_tasks_ = viewer_->recordAndSubmitTasks;
        seek_epoch(jump);
        // Drive display preparation for the pending seek without changing the
        // current rendered frame and its epoch-matched Earth rotation.
        const QScopedValueRollback snapshot_guard(snapshot_, sample_frame(jump));
        request_display_cache();
        smoke_require(display_candidate_.valid(),
                      "Uncached seek did not start display preparation");
        display_cancellation_->store(true);
        display_candidate_.wait();
        poll_display_cache();
        smoke_require(statusBar()->currentMessage().isEmpty(),
                      "Cancelled display preparation left a pending status");
        request_display_cache();
        smoke_require(display_candidate_.valid(), "Cancelled display preparation could not retry");
        statusBar()->showMessage("Newer operation feedback");
        display_cancellation_->store(true);
        display_candidate_.wait();
        poll_display_cache();
        smoke_require(statusBar()->currentMessage() == "Newer operation feedback",
                      "Cancelled display preparation erased a newer operation's status");
        smoke_record("display-preparation-cancel-clears-only-its-own-status");
        ++orbit_smoke_stage_;
        break;
    }
    case 52:
        if (!smoke_caches_ready())
            return;
        smoke_require(display_cache_publications_ > smoke_publications_ && orbit_ != smoke_cache_ &&
                          viewer_->recordAndSubmitTasks == smoke_render_tasks_ &&
                          orbit_->track.samples().states().size() < 200 &&
                          orbit_->session.provenance().input_revision ==
                              smoke_cache_->session.provenance().input_revision,
                      "Seeking did not refill a bounded cache without revising the orbit");
        smoke_record("uncached-seek-exact-position-and-asynchronous-period-cache");
        smoke_render_tasks_.clear();
        open_properties();
        editor_->findChild<QComboBox *>("trackExtent")->setCurrentIndex(1);
        editor_->findChild<QPushButton *>("applyDisplay")->click();
        properties_dialog_->close();
        ++orbit_smoke_stage_;
        break;
    case 53:
        if (!smoke_caches_ready())
            return;
        smoke_require(cache_covers(*orbit_, scenario_->first(), scenario_->last()) &&
                          orbit_->track.samples().states().size() == 1441,
                      "Explicit full-interval request did not prepare the day at 60 seconds");
        smoke_record("explicit-full-interval-cache-request");
        smoke_cache_ = orbit_;
        editor_->display({.track_2d = false, .track_3d = false});
        ++orbit_smoke_stage_;
        break;
    case 54:
        smoke_require(orbit_ == smoke_cache_ && !display_candidate_.valid(),
                      "Hiding trajectories launched another propagation");
        editor_->display({});
        smoke_require(orbit_ == smoke_cache_ && satellites_.front().inertial_track->size() < 160,
                      "One-period display failed to restrict the retained full cache");
        smoke_record("display-visibility-and-range-reuse-existing-cache");
        // These two checks called the workbench callback directly; synchronize the
        // editor explicitly instead of relying on reopening it to discard stale fields.
        editor_->set_display(satellites_.front().display);
        slots_[1]->dock->show();
        seek_epoch(scenario_->first());
        fit_views();
        fit_pending_ = true;
        smoke_wait_frame_ = frame_count_ + 3;
        ++orbit_smoke_stage_;
        break;
    case 55:
        // Complete the frame-driven surface lifecycle checks before measuring layout.
        if (frame_count_ < 150 || frame_count_ < smoke_wait_frame_ || !view_captures_ready())
            return;
        slots_[1]->dock->hide();
        open_properties();
        ++orbit_smoke_stage_;
        break;
    case 56:
        smoke_workspace_height_ = workspace_->height();
        smoke_timeline_epoch_ = timeline_->current_time();
        smoke_timeline_first_ = timeline_->visible_first();
        smoke_timeline_last_ = timeline_->visible_last();
        findChild<QToolButton *>("collapseTimeline")->click();
        ++orbit_smoke_stage_;
        break;
    case 57:
        save_workbench_image("workspace-collapsed-timeline.png");
        smoke_require(!timeline_->isVisible() && time_label_->isVisible() &&
                          play_action_->isEnabled(),
                      "Collapsing the timeline did not keep the transport controls visible");
        smoke_require(workspace_->height() >= smoke_workspace_height_ + 100,
                      qPrintable(QString("Collapsing the timeline did not release workspace "
                                         "height: %1 -> %2")
                                     .arg(smoke_workspace_height_)
                                     .arg(workspace_->height())));
        smoke_require(
            timeline_->current_time() == smoke_timeline_epoch_ &&
                timeline_->visible_first() == smoke_timeline_first_ &&
                timeline_->visible_last() == smoke_timeline_last_ &&
                editor_->findChild<QuantityEditor *>("displayStep")->value(Unit::second) == 60,
            "Collapsing the timeline changed the instant, range or property draft");
        findChild<QAction *>("expandTimeline")->setChecked(true);
        ++orbit_smoke_stage_;
        break;
    case 58: {
        smoke_require(timeline_->isVisible() && workspace_->height() == smoke_workspace_height_ &&
                          timeline_->current_time() == smoke_timeline_epoch_ &&
                          timeline_->visible_first() == smoke_timeline_first_ &&
                          timeline_->visible_last() == smoke_timeline_last_,
                      "Restoring the timeline changed its time or browsing range");
        smoke_record("timeline-collapse-restores-workspace-and-time-range");
        auto *show = findChild<QAction *>("showTimeline");
        auto *expand = findChild<QAction *>("expandTimeline");
        smoke_require(show && view_menu_->actions().contains(show) && show->isEnabled() &&
                          show->isChecked() && show->text() == QStringLiteral("时间轴"),
                      "View menu has no checked timeline recovery action");
        // The native toolbar context menu uses the same action as View -> Timeline.
        time_toolbar_->toggleViewAction()->trigger();
        smoke_require(!time_toolbar_->isVisible() && !show->isChecked() && !expand->isChecked(),
                      "Hidden timeline still reports itself as expanded");
        update_actions();
        smoke_require(!time_toolbar_->isVisible(),
                      "An ordinary action update reopened the hidden timeline");
        expand->trigger();
        smoke_require(time_toolbar_->isVisible() && timeline_->isVisible() && show->isChecked() &&
                          expand->isChecked(),
                      "Expand did not reopen the hidden timeline in one action");
        smoke_record("hidden-timeline-reopens-through-expand-command");
        findChild<QToolButton *>("collapseTimeline")->click();
        show->trigger();
        show->trigger();
        smoke_require(time_toolbar_->isVisible() && time_label_->isVisible() &&
                          !timeline_->isVisible() && !expand->isChecked(),
                      "View menu did not preserve the collapsed timeline controls");
        findChild<QToolButton *>("collapseTimeline")->click();
        smoke_require(
            timeline_->isVisible() && expand->isChecked() &&
                timeline_->current_time() == smoke_timeline_epoch_ &&
                timeline_->visible_first() == smoke_timeline_first_ &&
                timeline_->visible_last() == smoke_timeline_last_ &&
                editor_->findChild<QuantityEditor *>("displayStep")->value(Unit::second) == 60,
            "Reopening the timeline changed its time, range or property draft");
        smoke_record("timeline-view-menu-restores-controls-and-preserves-state");
        save_workbench_image("timeline-reopened-from-view-menu.png");
        properties_dialog_->close();
        slots_[1]->dock->show();
        fit_views();
        fit_pending_ = true;
        smoke_wait_frame_ = frame_count_ + 3;
        ++orbit_smoke_stage_;
        break;
    }
    case 59:
        if (frame_count_ < smoke_wait_frame_ || !view_captures_ready())
            return;
        slots_[1]->dock->hide();
        workspace_->select_view(0);
        smoke_wait_frame_ = frame_count_ + 3;
        ++orbit_smoke_stage_;
        break;
    case 60: {
        if (frame_count_ < smoke_wait_frame_ || !view_captures_ready())
            return;
        smoke_cache_ = orbit_;
        smoke_view_epoch_ = snapshot_.epoch;
        auto *look = dynamic_cast<vsg::LookAt *>(slots_[0]->camera->viewMatrix.get());
        smoke_camera_ = {look->eye, look->center, look->up};
        smoke_require(smoke_tabs(QStringLiteral("三维视图"))->count() == 2,
                      "Workspace still has a special parallel-view tab");
        smoke_drag_page(
            QStringLiteral("三维视图"),
            workspace_->mapToGlobal(QPoint(workspace_->width() - 12, workspace_->height() / 2)));
        smoke_wait_frame_ = frame_count_ + 3;
        ++orbit_smoke_stage_;
        break;
    }
    case 61: {
        if (frame_count_ < smoke_wait_frame_ || !view_captures_ready())
            return;
        smoke_require(ground_map_->isVisible() && slots_[0]->container->isVisible() &&
                          smoke_tabs(QStringLiteral("三维视图")) !=
                              smoke_tabs(QStringLiteral("二维视图")),
                      "Dragging the native 3D page did not split the workspace");
        auto *look = dynamic_cast<vsg::LookAt *>(slots_[0]->camera->viewMatrix.get());
        smoke_require(orbit_ == smoke_cache_ && snapshot_.epoch == smoke_view_epoch_ &&
                          look->eye == smoke_camera_[0] && look->center == smoke_camera_[1] &&
                          look->up == smoke_camera_[2],
                      "Dragging 3D changed the camera, current time or orbit cache");
        save_workbench_image("workspace-drag-split.png");
        smoke_record("drag-native-3d-tab-to-split-without-resetting-state");
        auto *tabs = smoke_tabs(QStringLiteral("二维视图"));
        smoke_drag_page(QStringLiteral("三维视图"),
                        tabs->mapToGlobal(QPoint(tabs->width() - 8, 16)));
        smoke_wait_frame_ = frame_count_ + 3;
        ++orbit_smoke_stage_;
        break;
    }
    case 62:
        if (frame_count_ < smoke_wait_frame_ || !view_captures_ready())
            return;
        smoke_require(smoke_tabs(QStringLiteral("三维视图")) ==
                              smoke_tabs(QStringLiteral("二维视图")) &&
                          !ground_map_->isVisible() && slots_[0]->container->isVisible(),
                      "Dragging 3D back to the tab bar did not merge the groups");
        smoke_record("drag-native-3d-tab-to-merge");
        smoke_drag_page(QStringLiteral("三维视图"), workspace_->mapToGlobal(QPoint(-80, 100)));
        smoke_wait_frame_ = frame_count_ + 3;
        ++orbit_smoke_stage_;
        break;
    case 63: {
        if (frame_count_ < smoke_wait_frame_ || !view_captures_ready())
            return;
        smoke_require(workspace_->is_floating(view_panel_) && slots_[0]->container->isVisible(),
                      "Dragging 3D out did not create a floating page");
        auto *window = view_panel_->window();
        smoke_require(window->width() >= 480 && window->height() >= 320,
                      qPrintable(QString("Floating 3D page is too small: %1 x %2")
                                     .arg(window->width())
                                     .arg(window->height())));
        auto picture = window->grab();
        QPainter painter(&picture);
        painter.drawImage(
            QRect(slots_[0]->container->mapTo(window, QPoint{}), slots_[0]->container->size()),
            slots_[0]->captured_image);
        painter.end();
        smoke_require(picture.save(smoke_dir_ + "/workspace-3d-floating.png"),
                      "Cannot capture floating 3D page");
        smoke_record("drag-native-3d-tab-to-floating-window");
        workspace_->dock_page(view_panel_);
        smoke_wait_frame_ = frame_count_ + 3;
        ++orbit_smoke_stage_;
        break;
    }
    case 64:
        if (frame_count_ < smoke_wait_frame_ || !view_captures_ready())
            return;
        smoke_require(!workspace_->is_floating(view_panel_), "3D page did not dock again");
        workspace_->close_page(view_panel_);
        smoke_require(!workspace_->contains(view_panel_), "3D page could not close");
        workspace_->select_view(0);
        smoke_wait_frame_ = frame_count_ + 3;
        ++orbit_smoke_stage_;
        break;
    case 65: {
        if (frame_count_ < smoke_wait_frame_ || !view_captures_ready())
            return;
        auto *look = dynamic_cast<vsg::LookAt *>(slots_[0]->camera->viewMatrix.get());
        smoke_require(workspace_->contains(view_panel_) && slots_[0]->container->isVisible() &&
                          orbit_ == smoke_cache_ && snapshot_.epoch == smoke_view_epoch_ &&
                          look->eye == smoke_camera_[0] && look->center == smoke_camera_[1] &&
                          look->up == smoke_camera_[2],
                      "Floating/closing/reopening 3D lost the camera, time or cached orbit");
        save_workbench_image("workspace-pages-restored.png");
        smoke_record("native-3d-float-dock-close-reopen-preserves-state");
        smoke_drag_page(
            QStringLiteral("二维视图"),
            workspace_->mapToGlobal(QPoint(workspace_->width() / 2, workspace_->height() - 12)));
        smoke_wait_frame_ = frame_count_ + 3;
        ++orbit_smoke_stage_;
        break;
    }
    case 66: {
        if (frame_count_ < smoke_wait_frame_ || !view_captures_ready())
            return;
        if (smoke_tabs(QStringLiteral("二维视图")) == smoke_tabs(QStringLiteral("三维视图"))) {
            smoke_require(workspace_->height() < 343 && ground_map_->isVisible() &&
                              workspace_->rect().contains(QRect(
                                  ground_map_->mapTo(workspace_, QPoint{}), ground_map_->size())),
                          "Vertical split was rejected with enough space or clipped the tab");
            save_workbench_image("workspace-short-tabs.png");
            smoke_record("insufficient-height-keeps-view-tabs-usable");
            workspace_->select_view(0);
            smoke_wait_frame_ = frame_count_ + 3;
            ++orbit_smoke_stage_;
            break;
        }
        smoke_require(ground_map_->isVisible() && slots_[0]->container->isVisible() &&
                          map_panel_->mapTo(workspace_, QPoint{}).y() >
                              view_panel_->mapTo(workspace_, QPoint{}).y(),
                      "Dragging to the bottom edge did not create a vertical split");
        const auto globe_rect =
            QRect(slots_[0]->container->mapTo(workspace_, QPoint{}), slots_[0]->container->size());
        const auto map_tabs = smoke_tabs(QStringLiteral("二维视图"));
        const auto map_rect = QRect(ground_map_->mapTo(workspace_, QPoint{}), ground_map_->size());
        const auto workspace_size = workspace_->size();
        const auto map_tabs_top = map_tabs->mapTo(workspace_, QPoint{}).y();
        const bool contained =
            globe_rect.bottom() < map_tabs_top && workspace_->rect().contains(map_rect);
        save_workbench_image("workspace-drag-vertical.png");
        smoke_require(contained,
                      qPrintable(QString("Vertical split clips the map tabs or the map content: "
                                         "workspace=%1x%2, globe bottom=%3, map tabs top=%4, "
                                         "map=(%5,%6,%7,%8)")
                                     .arg(workspace_size.width())
                                     .arg(workspace_size.height())
                                     .arg(globe_rect.bottom())
                                     .arg(map_tabs_top)
                                     .arg(map_rect.x())
                                     .arg(map_rect.y())
                                     .arg(map_rect.width())
                                     .arg(map_rect.height())));
        smoke_record("drag-pages-to-vertical-split");
        auto *tabs = smoke_tabs(QStringLiteral("三维视图"));
        smoke_drag_page(QStringLiteral("二维视图"),
                        tabs->mapToGlobal(QPoint(tabs->width() - 8, 16)));
        workspace_->select_view(0);
        smoke_wait_frame_ = frame_count_ + 3;
        ++orbit_smoke_stage_;
        break;
    }
    case 67: {
        if (frame_count_ < smoke_wait_frame_ || !view_captures_ready())
            return;
        auto *look = dynamic_cast<vsg::LookAt *>(slots_[0]->camera->viewMatrix.get());
        smoke_require(orbit_ == smoke_cache_ && snapshot_.epoch == smoke_view_epoch_ &&
                          look->eye == smoke_camera_[0] && look->center == smoke_camera_[1] &&
                          look->up == smoke_camera_[2],
                      "Vertical split/merge changed the camera, time or cached orbit");
        slots_[1]->dock->show();
        fit_views();
        smoke_wait_frame_ = frame_count_ + 3;
        ++orbit_smoke_stage_;
        break;
    }
    default:
        throw std::logic_error("Stage does not belong to display_cache_docking");
    }
}
