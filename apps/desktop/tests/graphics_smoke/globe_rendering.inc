void Workbench::smoke_globe_rendering() {
    constexpr double radius = 6378137.0;
    const int sample = (orbit_smoke_stage_ - 224) / 2;
    if (orbit_smoke_stage_ == 248) {
        // The final visibility checks need a lit disc even when the satellites
        // and fitted camera are on Earth's night side.
        lighting_action_->setChecked(false);
        change_display_frame(0);
        set_selection(0);
        fit_views();
        smoke_wait_frame_ = frame_count_ + 8;
        ++orbit_smoke_stage_;
        return;
    }
    if (orbit_smoke_stage_ == 249) {
        if (frame_count_ < smoke_wait_frame_)
            return;
        smoke_record("globe-polar-views-and-lighting");
        ++orbit_smoke_stage_;
        return;
    }
    const bool south = sample % 2 == 0;
    const int framing = sample % 6;
    const bool lit = framing < 2 || framing >= 4;
    if (orbit_smoke_stage_ % 2 == 0) {
        set_playing(false);
        change_display_frame(sample < 6 ? 0 : 1);
        lighting_action_->setChecked(lit);
        workspace_->select_view(0);
        const auto *perspective =
            dynamic_cast<vsg::Perspective *>(slots_[0]->camera->projectionMatrix.get());
        const auto extent = slots_[0]->window->windowAdapter->extent2D();
        // A fixed pixel radius keeps the far views on root tiles at every DPI.
        // Larger projected discs subdivide and hide the polar tangent defect.
        const double focal_pixels =
            extent.height / (2 * std::tan(perspective->fieldOfViewY * std::numbers::pi / 360));
        const double distance = framing < 4 ? std::hypot(1.0, focal_pixels / 96) : 2.4;
        slots_[0]->navigation->set_viewpoint(vsg::LookAt::create(
            earth_transform_->matrix *
                (vsg::normalize(vsg::dvec3(0.16, -0.10, south ? -1.0 : 1.0)) * distance * radius),
            vsg::dvec3(0, 0, 0), earth_transform_->matrix * vsg::dvec3(1, 0, 0)));
        fit_pending_ = false;
        orbit_capture_ = true;
        smoke_wait_frame_ = frame_count_ + 8;
        ++orbit_smoke_stage_;
        return;
    }
    if (frame_count_ < smoke_wait_frame_ || !view_captures_ready())
        return;
    const QString name = QString("globe-%1-%2-%3-%4.png")
                             .arg(sample < 6 ? "gcrf" : "itrf")
                             .arg(south ? "south" : "north")
                             .arg(lit ? "lit" : "unlit")
                             .arg(framing < 4 ? "far" : "near");
    smoke_require(slots_[0]->captured_image.save(smoke_dir_ + "/" + name),
                  "Cannot save polar globe swapchain image");
    if (!lit) {
        const auto &image = slots_[0]->captured_image;
        const double cx = image.width() / 2.0, cy = image.height() / 2.0;
        constexpr double interior_radius = 96 * .4;
        int interior_pixels = 0, black_pixels = 0;
        for (int y = static_cast<int>(cy - interior_radius); y <= cy + interior_radius; ++y)
            for (int x = static_cast<int>(cx - interior_radius); x <= cx + interior_radius; ++x) {
                if (std::hypot(x - cx, y - cy) >= interior_radius)
                    continue;
                ++interior_pixels;
                const auto color = image.pixelColor(x, y);
                // Both polar caps are bright in the fixed NASA imagery. Test
                // without lighting so the night side cannot mimic a black slit.
                if (std::max({color.red(), color.green(), color.blue()}) < 8)
                    ++black_pixels;
            }
        interaction_report_.append(QJsonObject{{"kind", "globe-polar-interior"},
                                               {"image", name},
                                               {"interior_pixels", interior_pixels},
                                               {"black_pixels", black_pixels},
                                               {"passed", black_pixels == 0}});
        smoke_require(interior_pixels > 4000 && black_pixels == 0,
                      "Unlit polar terrain contains black slits");
    }
    ++orbit_smoke_stage_;
}
