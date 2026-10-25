#include "../src/viewer/ground_map.hpp"
#include "../src/viewer/workbench_profile.hpp"
#include <QApplication>
#include <QDir>
#include <QElapsedTimer>
#include <QFocusEvent>
#include <QKeyEvent>
#include <QLineF>
#include <QMouseEvent>
#include <QTemporaryDir>
#include <QThread>
#include <QWheelEvent>
#include <cmath>
#include <gtest/gtest.h>
#include <limits>
#include <numbers>

namespace orbelle::desktop {
TEST(GroundFootprint, SplitsDateLineAndClosesPolarCapWithoutFillingOtherHemisphere) {
    const auto p = [](double lon, double lat) {
        return core::ground_station_position(
            {lon * std::numbers::pi / 180, lat * std::numbers::pi / 180, 0});
    };
    const std::vector<core::SensorTriangle> seam{{p(175, -5), p(-175, -5), p(180, 5)}};
    const auto path = ground_footprint_path(seam);
    EXPECT_TRUE(path.contains({179, 0}));
    EXPECT_TRUE(path.contains({-179, 0}));
    EXPECT_FALSE(path.contains({0, 0}));
    EXPECT_FALSE(path.contains({170, 0}));
    const std::vector<core::SensorTriangle> polar{{p(-120, 85), p(0, 85), p(120, 85)}};
    const auto cap = ground_footprint_path(polar);
    EXPECT_TRUE(cap.contains({179, 89}));
    EXPECT_TRUE(cap.contains({-179, 89}));
    EXPECT_TRUE(cap.contains({0, 89}));
    EXPECT_FALSE(cap.contains({0, 70}));
    EXPECT_FALSE(cap.contains({0, -89}));
}
TEST(GroundFootprint, ObliqueStationTrackingFillMeetsEveryVisibleLimbSegment) {
    constexpr double degree = std::numbers::pi / 180;
    const auto surface = core::ground_station_position({-122.315 * degree, .151 * degree, 0});
    const core::SensorVector satellite{surface[0] * 7e6 / 6378137., surface[1] * 7e6 / 6378137.,
                                       surface[2] * 7e6 / 6378137.};
    const auto station = core::ground_station_position({-105 * degree, 14 * degree, 0});
    const auto axes =
        core::sensor_axes({5000, 3000, 5000}, {station[0] - satellite[0], station[1] - satellite[1],
                                               station[2] - satellite[2]});
    const core::SensorPattern pattern(45 * degree);
    const core::SensorGeometry geometry(pattern);
    const auto mesh = visualization::SensorView(pattern).project(satellite, axes, 2e7);
    const auto path = ground_footprint_path(mesh.ground);
    const auto subpoint = ground_coordinates(satellite);
    std::size_t checked = 0, gaps = 0;
    for (const auto &edge : mesh.ground_edges) {
        const core::SensorVector middle{(edge[0][0] + edge[1][0]) / 2 - satellite[0],
                                        (edge[0][1] + edge[1][1]) / 2 - satellite[1],
                                        (edge[0][2] + edge[1][2]) / 2 - satellite[2]};
        if (geometry.margin(axes.to_local(middle)) < .03)
            continue;
        const auto point = (ground_coordinates(edge[0]) + ground_coordinates(edge[1])) / 2;
        // Stay just inside the displayed outline, well below one pixel at normal zoom.
        gaps += !path.contains(point + (subpoint - point) * 1e-4);
        ++checked;
    }
    ASSERT_GT(checked, 20u);
    EXPECT_EQ(gaps, 0u);
}
TEST(GroundLightingMask, MatchesPerPixelReferenceAtPolesSeamsAndTwilight) {
    for (int sample = 0; sample < 20; ++sample) {
        const double longitude = sample * std::numbers::pi / 5;
        const double latitude = (sample % 5 - 2) * std::numbers::pi / 4;
        const std::array<double, 3> direction{std::cos(latitude) * std::cos(longitude),
                                              std::cos(latitude) * std::sin(longitude),
                                              std::sin(latitude)};
        const auto actual = ground_lighting_mask(direction);
        QImage reference(1440, 720, QImage::Format_ARGB32_Premultiplied);
        std::array<double, 1440> horizontal;
        for (int x = 0; x < reference.width(); ++x) {
            const double lon = ((x + 0.5) / reference.width() * 360 - 180) * std::numbers::pi / 180;
            horizontal[x] = std::cos(lon) * direction[0] + std::sin(lon) * direction[1];
        }
        // Deliberately retain the old exhaustive pixel evaluation as an oracle.
        for (int y = 0; y < reference.height(); ++y) {
            const double lat = (90 - (y + 0.5) / reference.height() * 180) * std::numbers::pi / 180;
            const double cosine_latitude = std::cos(lat), axial = std::sin(lat) * direction[2];
            auto *pixels = reinterpret_cast<QRgb *>(reference.scanLine(y));
            for (int x = 0; x < reference.width(); ++x) {
                const double cosine = cosine_latitude * horizontal[x] + axial;
                const double t = std::clamp(0.5 - cosine / 0.006, 0.0, 1.0);
                const double night = t * t * (3 - 2 * t);
                pixels[x] = qRgb(qRound(255 - 213 * night), qRound(255 - 191 * night),
                                 qRound(255 - 165 * night));
            }
        }
        EXPECT_EQ(actual, reference) << "direction " << sample;
    }
    EXPECT_THROW(ground_lighting_mask({0, 0, 0}), std::invalid_argument);
    EXPECT_THROW(ground_lighting_mask({std::numeric_limits<double>::infinity(), 0, 0}),
                 std::invalid_argument);
}
class GroundMapTest : public ::testing::Test {
  protected:
    void SetUp() override {
        if (!QApplication::instance()) {
            static int argc = 1;
            static char executable[] = "map_test";
            static char *argv[] = {executable, nullptr};
            static QApplication app(argc, argv);
        }
        ASSERT_TRUE(directory_.isValid());
        QImage image(720, 360, QImage::Format_RGB32);
        for (int y = 0; y < image.height(); ++y)
            for (int x = 0; x < image.width(); ++x)
                image.setPixelColor(x, y, QColor(25 + x / 10, 40 + y / 5, 70 + (x + y) / 12));
        // Match the bundled Earth imagery format, including its Qt codec dependency.
        imagery_ = directory_.filePath("earth.jpg");
        ASSERT_TRUE(image.save(imagery_));
        ASSERT_FALSE(QImage(imagery_).isNull());
    }
    static GroundMap::Track track() {
        GroundMap::Track result;
        result.name = "Satellite";
        result.points = {{-175, -40}, {-20, 35}, {15, 20}, {165, -40}, {-160, 30}};
        for (int i = 0; i < 5; ++i)
            result.epochs.emplace_back(i * 10);
        result.last = result.points.size();
        result.path = ground_path(result.points);
        return result;
    }
    static void configure(GroundMap &map) {
        map.resize(640, 400);
        map.publish_tracks({track()});
        map.set_snapshot({core::Epoch{10},
                          core::FrameId::itrf,
                          core::OriginId::earth,
                          {{"Satellite", {7e6, 0, 0}}}});
        map.set_sun_direction({1, 0, 0});
    }
    static QImage pixels(GroundMap &map) { return map.grab().toImage(); }
    static void capture(const QImage &image, const char *suffix) {
        const auto output = qEnvironmentVariable("ORBELLE_MAP_CAPTURE_DIR");
        if (output.isEmpty())
            return;
        ASSERT_TRUE(QDir().mkpath(output));
        const auto name =
            QString::fromUtf8(::testing::UnitTest::GetInstance()->current_test_info()->name());
        EXPECT_TRUE(image.save(output + "/" + name + "-" + QString::fromUtf8(suffix) + ".png"));
    }
    static GroundMap::Track dense_track() {
        auto result = track();
        result.points.clear();
        result.epochs.clear();
        for (int i = 0; i < 4000; ++i) {
            result.points.emplace_back(std::remainder(i * 0.7, 360.0), 50 * std::sin(i * 0.017));
            result.epochs.emplace_back(i * 10);
        }
        result.last = result.points.size();
        result.path = ground_path(result.points);
        return result;
    }
    static bool settle(GroundMap &map) {
        QElapsedTimer deadline;
        deadline.start();
        do {
            QCoreApplication::processEvents();
            (void)pixels(map);
            if (map.tracks_ready() && !map.tracks_transitioning() && map.footprints_ready() &&
                map.background_ready())
                return true;
            QThread::msleep(1);
        } while (deadline.elapsed() < 3000);
        return false;
    }
    static void expect_same_pixels(const QImage &a, const QImage &b) {
        ASSERT_EQ(a.size(), b.size());
        int maximum = 0;
        double total = 0;
        for (int y = 0; y < a.height(); ++y)
            for (int x = 0; x < a.width(); ++x) {
                const auto p = a.pixelColor(x, y), q = b.pixelColor(x, y);
                const int difference =
                    std::max({std::abs(p.red() - q.red()), std::abs(p.green() - q.green()),
                              std::abs(p.blue() - q.blue())});
                maximum = std::max(maximum, difference);
                total += difference;
            }
        // Halo, stroke and image composition each quantize to 8-bit channels.
        // At DPR 1.5 the original renderer already differs by 3/255 at two edge
        // pixels when translating a cached path versus rasterizing it afresh.
        // Also bound the mean so a widespread tint or stale layer cannot pass.
        EXPECT_LE(maximum, 3);
        EXPECT_LE(total / (a.width() * a.height()), 0.01);
        const auto output = qEnvironmentVariable("ORBELLE_MAP_CAPTURE_DIR");
        if (maximum > 3 && !output.isEmpty()) {
            QDir().mkpath(output);
            const auto name =
                QString::fromUtf8(::testing::UnitTest::GetInstance()->current_test_info()->name());
            (void)a.save(output + "/" + name + "-cached.png");
            (void)b.save(output + "/" + name + "-fresh.png");
        }
    }
    static void pan(GroundMap &map) {
        QMouseEvent press(QEvent::MouseButtonPress, QPointF(300, 180), QPointF(300, 180),
                          Qt::LeftButton, Qt::LeftButton, Qt::NoModifier);
        QMouseEvent move(QEvent::MouseMove, QPointF(330, 200), QPointF(330, 200), Qt::NoButton,
                         Qt::LeftButton, Qt::NoModifier);
        QMouseEvent release(QEvent::MouseButtonRelease, QPointF(330, 200), QPointF(330, 200),
                            Qt::LeftButton, Qt::NoButton, Qt::NoModifier);
        QCoreApplication::sendEvent(&map, &press);
        QCoreApplication::sendEvent(&map, &move);
        QCoreApplication::sendEvent(&map, &release);
    }
    static void mouse(GroundMap &map, QEvent::Type type, QPointF point,
                      Qt::MouseButton button = Qt::LeftButton,
                      Qt::KeyboardModifiers modifiers = Qt::NoModifier) {
        QMouseEvent event(type, point, point, type == QEvent::MouseMove ? Qt::NoButton : button,
                          type == QEvent::MouseButtonRelease ? Qt::NoButton : button, modifiers);
        QCoreApplication::sendEvent(&map, &event);
    }
    static void drag(GroundMap &map, QPointF delta) {
        const auto origin = QRectF(map.rect()).adjusted(38, 14, -16, -44).center();
        mouse(map, QEvent::MouseButtonPress, origin, Qt::MiddleButton);
        mouse(map, QEvent::MouseMove, origin + delta, Qt::MiddleButton);
        mouse(map, QEvent::MouseButtonRelease, origin + delta, Qt::MiddleButton);
        QEvent leave(QEvent::Leave);
        QCoreApplication::sendEvent(&map, &leave);
    }
    static core::SensorVector position(double longitude, double latitude) {
        return core::ground_station_position(
            {longitude * std::numbers::pi / 180, latitude * std::numbers::pi / 180, 0});
    }
    static visualization::SensorMesh sensor_patch(double longitude, double half_width = 6) {
        visualization::SensorMesh mesh;
        const auto left = longitude - half_width, right = longitude + half_width;
        mesh.ground = {{position(left, -6), position(right, -6), position(right, 6)},
                       {position(left, -6), position(right, 6), position(left, 6)}};
        mesh.color = 0xff00ff;
        mesh.opacity = .7;
        return mesh;
    }
    static void configure_sensor_map(GroundMap &map) {
        map.resize(640, 400);
        map.set_labels_visible(false);
        map.set_grid_visible(false);
        map.set_lighting(false);
        map.set_sun_visible(false);
    }
    static std::pair<int, double> sensor_pixels(const QImage &image) {
        int count = 0;
        double horizontal = 0;
        const auto ratio = image.devicePixelRatio();
        for (int y = qCeil(14 * ratio); y < image.height() - 44 * ratio; ++y)
            for (int x = qCeil(38 * ratio); x < image.width() - 16 * ratio; ++x) {
                const auto color = image.pixelColor(x, y);
                if (color.red() > 140 && color.blue() > 140 && color.green() < 70) {
                    ++count;
                    horizontal += x;
                }
            }
        return {count, count ? horizontal / count : 0};
    }
    static bool wheel(GroundMap &map, QPointF point, QPoint angle, QPoint pixel = {}) {
        QWheelEvent event(point, point, pixel, angle, Qt::NoButton, Qt::NoModifier,
                          pixel.isNull() ? Qt::NoScrollPhase : Qt::ScrollUpdate, false);
        QCoreApplication::sendEvent(&map, &event);
        return event.isAccepted();
    }
    static void key(GroundMap &map, int code, Qt::KeyboardModifiers modifiers = Qt::NoModifier) {
        QKeyEvent event(QEvent::KeyPress, code, modifiers);
        QCoreApplication::sendEvent(&map, &event);
    }
    static void process_for(int milliseconds) {
        QElapsedTimer timer;
        timer.start();
        do {
            QCoreApplication::processEvents();
            QThread::msleep(1);
        } while (timer.elapsed() < milliseconds);
    }
    static bool wait_zoom(GroundMap &map, double expected) {
        QElapsedTimer timer;
        timer.start();
        do {
            QCoreApplication::processEvents();
            if (std::abs(map.zoom_factor() - expected) < 1e-9)
                return true;
            QThread::msleep(1);
        } while (timer.elapsed() < 1000);
        return false;
    }
    QTemporaryDir directory_;
    QString imagery_;
};
TEST_F(GroundMapTest, ShortViewportFillsWithoutStretchingAndCanPanOrShowTheWholeWorld) {
    GroundMap map(imagery_);
    EXPECT_TRUE(map.toolTip().isEmpty());
    EXPECT_FALSE(map.accessibleDescription().isEmpty());
    map.resize(1000, 110);
    const auto p = [](double longitude, double latitude) {
        return core::ground_station_position(
            {longitude * std::numbers::pi / 180, latitude * std::numbers::pi / 180, 0});
    };
    map.set_snapshot({core::Epoch{10},
                      core::FrameId::itrf,
                      core::OriginId::earth,
                      {{"Origin", p(0, 0)},
                       {"East", p(1, 0)},
                       {"North", p(0, 1)},
                       {"West edge", p(-179, 0)},
                       {"East edge", p(179, 0)},
                       {"South edge", p(0, -89)},
                       {"North edge", p(0, 89)}}});
    map.set_lighting(false);
    map.set_sun_visible(false);
    map.set_grid_visible(false);
    map.set_labels_visible(false);
    const auto image = pixels(map);
    const auto plot = QRectF(map.rect()).adjusted(38, 14, -16, -44);
    const auto inset = plot.adjusted(2, 2, -2, -2);
    for (const auto point :
         {inset.topLeft(), inset.topRight(), inset.bottomLeft(), inset.bottomRight()}) {
        const auto pixel = (point * map.devicePixelRatioF()).toPoint();
        EXPECT_NE(image.pixelColor(pixel), image.pixelColor(0, 0));
    }
    const auto origin = map.marker_position(0);
    EXPECT_NEAR(map.marker_position(1).x() - origin.x(), origin.y() - map.marker_position(2).y(),
                1e-8);
    mouse(map, QEvent::MouseButtonPress, origin, Qt::MiddleButton);
    mouse(map, QEvent::MouseMove, origin + QPointF(0, 20), Qt::MiddleButton);
    mouse(map, QEvent::MouseButtonRelease, origin + QPointF(0, 20), Qt::MiddleButton);
    EXPECT_NEAR(map.marker_position(0).y(), origin.y() + 20, 1e-8);
    map.zoom(0.0001, plot.center());
    EXPECT_LT(map.zoom_factor(), 1);
    for (std::size_t i = 3; i < 7; ++i)
        EXPECT_TRUE(plot.contains(map.marker_position(i)));
    map.reset_view();
    EXPECT_EQ(map.zoom_factor(), 1);
    EXPECT_NEAR(QLineF(map.marker_position(0), origin).length(), 0, 1e-8);
}
TEST_F(GroundMapTest, ProfileZoomedSensorPanning) {
    const auto output = qEnvironmentVariable("ORBELLE_SENSOR_PAN_PROFILE");
    if (output.isEmpty())
        GTEST_SKIP() << "Opt-in pan benchmark";
    WorkbenchProfile profile(output);
    constexpr int detail_rebuilds = 20;
    constexpr double detail_pan_pixels = 240;
    double device_ratio = 1;
    for (int mode = 0; mode < 3; ++mode) {
        GroundMap map(imagery_);
        std::optional<visualization::SensorView> sensor;
        configure(map);
        map.resize(1280, 800);
        if (mode) {
            core::SensorPattern pattern;
            pattern.half_angle_rad = (mode == 1 ? 45 : 80) * std::numbers::pi / 180;
            sensor.emplace(pattern);
            const core::SensorVector position{mode == 1 ? 7e6 : 42e6, 0, 0};
            const auto axes = core::sensor_axes({0, 7500, 0}, {-1, 0, 0});
            const auto mesh = sensor->project(position, axes, 20e6);
            std::cout << "pan mode " << mode << ": " << mesh.ground.size() << " facets\n"
                      << std::flush;
            map.set_sensor_footprints(std::span(&mesh, 1));
        }
        map.show();
        QCoreApplication::processEvents();
        ASSERT_EQ(map.size(), QSize(1280, 800));
        if (mode == 0)
            device_ratio = map.devicePixelRatioF();
        else
            ASSERT_EQ(map.devicePixelRatioF(), device_ratio);
        map.zoom(12, map.rect().center());
        ASSERT_TRUE(settle(map));
        profile.phase(QString("pan-%1").arg(mode), true);
        mouse(map, QEvent::MouseButtonPress, {400, 300});
        for (int step = 1; step <= 30; ++step) {
            mouse(map, QEvent::MouseMove, {400.0 + step * 3, 300.0 + step});
            (void)pixels(map);
        }
        mouse(map, QEvent::MouseButtonRelease, {670, 390});
        profile.phase("settle", false);
        profile.finish({{"completed_mode", mode}});
        ASSERT_TRUE(settle(map));
        profile.phase(QString("pan-beyond-cache-%1").arg(mode), true);
        mouse(map, QEvent::MouseButtonPress, {400, 300});
        for (int step = 1; step <= 30; ++step) {
            mouse(map, QEvent::MouseMove, {400.0 + step * 12, 300.0 + step * 2});
            (void)pixels(map);
        }
        mouse(map, QEvent::MouseButtonRelease, {760, 360});
        profile.phase("settle", false);
        ASSERT_TRUE(settle(map));

        // Use the same scale and geometry around the sensor's original center.
        // Drain earlier work before measuring so its completion cannot be
        // attributed to the new detail-rebuild phase.
        map.reset_view();
        map.zoom(12, map.rect().center());
        ASSERT_TRUE(settle(map));
        profile.phase(QString("detail-rebuild-%1").arg(mode), true);
        for (int rebuild = 0; rebuild < detail_rebuilds; ++rebuild) {
            SCOPED_TRACE(rebuild);
            QElapsedTimer latency;
            latency.start();
            // Each displacement exceeds the 160-pixel overscan. Alternating
            // between nearby viewports keeps coverage in view, but only the
            // latest detail raster is cached, so every step needs a fresh one.
            drag(map, {rebuild % 2 == 0 ? detail_pan_pixels : -detail_pan_pixels, 0});
            ASSERT_FALSE(map.tracks_ready());
            if (mode)
                ASSERT_FALSE(map.footprints_ready());
            ASSERT_TRUE(settle(map));
            profile.record("map_detail_settle_ms", latency.nsecsElapsed() * 1e-6);
        }
        profile.phase("settle", false);
        for (const auto zoom : {2, 12}) {
            map.reset_view();
            map.zoom(zoom, QRectF(map.rect()).adjusted(38, 14, -16, -44).center());
            ASSERT_TRUE(settle(map));
            profile.phase(QString("playback-zoom-%1-mode-%2").arg(zoom).arg(mode), true);
            for (int step = 0; step < 48; ++step) {
                {
                    const ProfileScope frame("map_playback_frame_ms");
                    const auto angle = step * .0005;
                    const auto radius = mode == 2 ? 42e6 : 7e6;
                    const core::SensorVector position{radius * std::cos(angle),
                                                      radius * std::sin(angle), 0};
                    const core::SensorVector velocity{-7500 * std::sin(angle),
                                                      7500 * std::cos(angle), 0};
                    if (sensor) {
                        visualization::SensorMesh mesh;
                        {
                            const ProfileScope projection("sensor_projection_ms");
                            mesh = sensor->project(
                                position,
                                core::sensor_axes(velocity, {-position[0], -position[1], 0}), 20e6);
                        }
                        map.set_sensor_footprints(std::span(&mesh, 1), true);
                    }
                    map.set_snapshot({core::Epoch{step * 1'000'000'000LL},
                                      core::FrameId::itrf,
                                      core::OriginId::earth,
                                      {{"Satellite", position}}});
                    if (step == 12 || step == 28)
                        map.zoom_centered(step == 12 ? 2 : .5);
                    QCoreApplication::processEvents();
                    (void)pixels(map);
                }
                QThread::msleep(8);
            }
            profile.phase("settle", false);
            ASSERT_TRUE(settle(map));
        }
    }
    profile.finish({{"logical_width", 1280},
                    {"logical_height", 800},
                    {"device_pixel_ratio", device_ratio},
                    {"qt_platform", QGuiApplication::platformName()},
                    {"zoom", 12},
                    {"steps", 30},
                    {"detail_rebuilds", detail_rebuilds},
                    {"detail_pan_pixels", detail_pan_pixels}});
}
TEST_F(GroundMapTest, SensorRasterKeepsHolesAndRefreshesAfterPanResizeAndRemoval) {
    GroundMap map(imagery_), reference(imagery_);
    for (auto *view : {&map, &reference}) {
        configure(*view);
        view->set_labels_visible(false);
        view->set_grid_visible(false);
        view->set_lighting(false);
        view->set_tracks_visible(false);
        view->set_sun_visible(false);
        view->zoom(12, view->rect().center());
    }
    ASSERT_TRUE(settle(reference));
    const auto background = pixels(reference);
    core::SensorPattern pattern;
    pattern.type = core::SensorPatternType::complex_conic;
    pattern.half_angle_rad = .8;
    pattern.inner_half_angle_rad = .3;
    const visualization::SensorView geometry(pattern);
    auto mesh = geometry.project({7e6, 0, 0}, core::sensor_axes({0, 7500, 0}, {-1, 0, 0}), 20e6);
    mesh.color = 0xff0066;
    mesh.opacity = .7;
    map.set_sensor_footprints(std::span(&mesh, 1));
    ASSERT_TRUE(settle(map));
    auto frame = pixels(map);
    EXPECT_NE(frame, background);
    // The central blind area stays transparent, including after a high-DPI cache.
    const auto center = map.marker_position(0) * map.devicePixelRatioF();
    for (int y = -6; y <= 6; ++y)
        for (int x = -6; x <= 6; ++x)
            EXPECT_EQ(frame.pixelColor(center.toPoint() + QPoint(x, y)),
                      background.pixelColor(center.toPoint() + QPoint(x, y)));
    for (auto *view : {&map, &reference}) {
        mouse(*view, QEvent::MouseButtonPress, {300, 180});
        mouse(*view, QEvent::MouseMove, {90.25, 220.5});
        (void)pixels(*view);
        mouse(*view, QEvent::MouseButtonRelease, {90.25, 220.5});
        view->resize(780, 440);
    }
    reference.set_sensor_footprints(std::span(&mesh, 1));
    ASSERT_TRUE(settle(map));
    ASSERT_TRUE(settle(reference));
    expect_same_pixels(pixels(map), pixels(reference));
    // Replacing an in-flight request cannot publish the previous shape or color.
    mesh.color = 0x00ddff;
    map.set_sensor_footprints(std::span(&mesh, 1));
    (void)pixels(map);
    map.set_sensor_footprints({});
    reference.set_sensor_footprints({});
    process_for(80);
    ASSERT_TRUE(settle(map));
    ASSERT_TRUE(settle(reference));
    expect_same_pixels(pixels(map), pixels(reference));
}
TEST_F(GroundMapTest, SensorPlaybackPublishesMovingFramesWithoutClearingCompletedCoverage) {
    GroundMap map(imagery_), reference(imagery_);
    configure_sensor_map(map);
    configure_sensor_map(reference);
    auto mesh = sensor_patch(-20);
    bool visible = false;
    double first_center = 0, last_center = 0;
    // Start playback before any raster exists. Each new position arrives without
    // waiting for its worker, as it does on the workbench's shared frame timer.
    for (int i = 0; i < 48; ++i) {
        mesh = sensor_patch(-20 + i * .5);
        map.set_sensor_footprints(std::span(&mesh, 1), true);
        const auto frame = pixels(map);
        const auto [count, center] = sensor_pixels(frame);
        if (visible)
            EXPECT_GT(count, 100) << "coverage disappeared at playback frame " << i;
        if (count > 100) {
            if (!visible) {
                first_center = center;
                capture(frame, "first-playback-coverage");
            }
            visible = true;
            last_center = center;
        }
        process_for(8);
    }
    ASSERT_TRUE(visible) << "continuous updates starved the footprint worker";
    EXPECT_GT(last_center - first_center, 10 * map.devicePixelRatioF());
    capture(pixels(map), "moving-coverage");
    ASSERT_TRUE(settle(map));
    reference.set_sensor_footprints(std::span(&mesh, 1));
    ASSERT_TRUE(settle(reference));
    expect_same_pixels(pixels(map), pixels(reference));
}
TEST_F(GroundMapTest, SensorCoverageSurvivesZoomBurstsBeyondTheOverviewScale) {
    QImage imagery(720, 360, QImage::Format_RGB32);
    imagery.fill(QColor(90, 110, 130));
    GroundMap map(imagery), reference(imagery);
    for (auto *view : {&map, &reference}) {
        configure_sensor_map(*view);
        view->show();
        view->zoom(12, QRectF(view->rect()).adjusted(38, 14, -16, -44).center());
    }
    auto mesh = sensor_patch(0, 80);
    map.set_sensor_footprints(std::span(&mesh, 1));
    ASSERT_TRUE(settle(map));
    const auto plot = QRectF(map.rect()).adjusted(38, 14, -16, -44);
    const auto center = (plot.center() * map.devicePixelRatioF()).toPoint();
    const auto coverage = pixels(map).pixelColor(center);
    ASSERT_GT(coverage.red(), 140);
    // A large wheel burst can outgrow the old detail before a worker completes.
    map.zoom(4, plot.center());
    EXPECT_EQ(pixels(map).pixelColor(center), coverage);
    for (const auto factor : {.25, 4.0, .25}) {
        map.zoom_centered(factor);
        QElapsedTimer animation;
        animation.start();
        do {
            EXPECT_EQ(pixels(map).pixelColor(center), coverage) << "zoom=" << map.zoom_factor();
            process_for(8);
        } while (animation.elapsed() < 160);
    }
    capture(pixels(map), "zoomed-coverage");
    ASSERT_TRUE(settle(map));
    reference.zoom(map.zoom_factor() / reference.zoom_factor(), plot.center());
    reference.set_sensor_footprints(std::span(&mesh, 1));
    ASSERT_TRUE(settle(reference));
    expect_same_pixels(pixels(map), pixels(reference));
}
TEST_F(GroundMapTest, ReplacingSensorsDiscardsInFlightPlaybackCoverage) {
    GroundMap map(imagery_), reference(imagery_);
    configure_sensor_map(map);
    configure_sensor_map(reference);
    auto mesh = sensor_patch(-30);
    map.set_sensor_footprints(std::span(&mesh, 1));
    ASSERT_TRUE(settle(map));
    ASSERT_GT(sensor_pixels(pixels(map)).first, 100);
    mesh = sensor_patch(-29);
    map.set_sensor_footprints(std::span(&mesh, 1), true);
    (void)pixels(map);
    // Replace the collection while a playback request is running. A surviving
    // sensor must not allow the removed/edited sensor's old result to reappear.
    mesh = sensor_patch(30);
    mesh.color = 0x00ddff;
    map.set_sensor_footprints(std::span(&mesh, 1));
    for (int i = 0; i < 8; ++i) {
        EXPECT_EQ(sensor_pixels(pixels(map)).first, 0);
        process_for(8);
    }
    ASSERT_TRUE(settle(map));
    reference.set_sensor_footprints(std::span(&mesh, 1));
    ASSERT_TRUE(settle(reference));
    expect_same_pixels(pixels(map), pixels(reference));
}
TEST_F(GroundMapTest, ObliqueSensorFillHasNoInternalRasterSeams) {
    QImage black(720, 360, QImage::Format_RGB32);
    black.fill(Qt::black);
    GroundMap map(black);
    map.resize(1000, 700);
    map.set_grid_visible(false);
    map.set_lighting(false);
    map.set_sun_visible(false);
    map.set_labels_visible(false);
    map.set_snapshot(
        {core::Epoch{0},
         core::FrameId::itrf,
         core::OriginId::earth,
         {{"origin", {6378137, 0, 0}}, {"east", core::ground_station_position({.1, 0, 0})}}});
    map.zoom_centered(4);
    const core::SensorVector satellite{7e6, 0, 0};
    const auto station = core::ground_station_position({.3, .2, 0});
    const auto axes =
        core::sensor_axes({0, 5000, 5000}, {station[0] - satellite[0], station[1], station[2]});
    const core::SensorPattern pattern(std::numbers::pi / 4);
    const core::SensorGeometry geometry(pattern);
    auto mesh = visualization::SensorView(pattern).project(satellite, axes, 2e7);
    mesh.color = 0xffffff;
    mesh.opacity = 1;
    mesh.ground_edges.clear();
    map.set_sensor_footprints(std::span(&mesh, 1));
    ASSERT_TRUE(settle(map));
    const auto frame = pixels(map);
    capture(frame, "seamless-fill");
    const auto origin = map.marker_position(0) * map.devicePixelRatioF();
    const auto east = map.marker_position(1) * map.devicePixelRatioF();
    const auto radians_per_pixel =
        .1 / ((map.marker_position(1).x() - map.marker_position(0).x()) * map.devicePixelRatioF());
    int checked = 0, minimum = 255;
    // Sample comfortably inside the analytic visible footprint, away from the
    // markers and its antialiased boundary. Shared facet edges must stay opaque.
    for (int y = 40; y < frame.height() - 90; ++y)
        for (int x = 70; x < frame.width() - 40; ++x) {
            if (QLineF(QPointF(x, y), origin).length() < 12 * map.devicePixelRatioF() ||
                QLineF(QPointF(x, y), east).length() < 12 * map.devicePixelRatioF())
                continue;
            const auto point =
                core::ground_station_position({(x + .5 - origin.x()) * radians_per_pixel,
                                               (origin.y() - y - .5) * radians_per_pixel, 0});
            const core::SensorVector ray{point[0] - satellite[0], point[1], point[2]};
            if (point[0] < 5.95e6 || geometry.margin(axes.to_local(ray)) < .03)
                continue;
            minimum = std::min(minimum, frame.pixelColor(x, y).red());
            ++checked;
        }
    ASSERT_GT(checked, 1000);
    EXPECT_GE(minimum, 254);
}
TEST_F(GroundMapTest, TrackStyleChangesRepaintCachedGeometryWithColorWidthAndGaps) {
    GroundMap map(imagery_);
    configure(map);
    map.set_lighting(false);
    map.set_grid_visible(false);
    map.set_labels_visible(false);
    map.set_sun_visible(false);
    DisplaySettings settings;
    settings.line_color = 0xff3040;
    settings.line_width = 1;
    const auto red_pixels = [&](const QImage &image) {
        std::size_t count = 0;
        for (int y = 0; y < image.height(); ++y)
            for (int x = 0; x < image.width(); ++x) {
                const auto c = image.pixelColor(x, y);
                if (c.red() > 180 && c.green() < 100 && c.blue() < 110)
                    ++count;
            }
        return count;
    };
    map.set_display(0, settings, core::Epoch{0}, core::Epoch{40});
    ASSERT_TRUE(settle(map));
    const auto thin = red_pixels(pixels(map));
    settings.line_width = 6;
    map.set_display(0, settings, core::Epoch{0}, core::Epoch{40});
    ASSERT_TRUE(settle(map));
    const auto thick = red_pixels(pixels(map));
    EXPECT_GT(thin, 100u);
    EXPECT_GT(thick, thin * 3);
    settings.line_style = application::LineStyle::dotted;
    map.set_display(0, settings, core::Epoch{0}, core::Epoch{40});
    ASSERT_TRUE(settle(map));
    const auto dotted = red_pixels(pixels(map));
    EXPECT_GT(dotted, thick / 6);
    EXPECT_LT(dotted, thick * 2 / 3);
    settings.line_color = 0x73cdee;
    map.set_display(0, settings, core::Epoch{0}, core::Epoch{40});
    ASSERT_TRUE(settle(map));
    EXPECT_EQ(red_pixels(pixels(map)), 0u);
}
TEST_F(GroundMapTest, RangeVisibilityReplacementAndSelectionRepaint) {
    GroundMap map(imagery_);
    configure(map);
    const auto full = pixels(map);
    map.set_display(0, {}, core::Epoch{10}, core::Epoch{20});
    EXPECT_NE(pixels(map), full);
    map.set_display(0, {}, core::Epoch{0}, core::Epoch{40});
    EXPECT_EQ(pixels(map), full);
    map.set_tracks_visible(false);
    const auto hidden = pixels(map);
    EXPECT_NE(hidden, full);
    map.set_tracks_visible(true);
    EXPECT_EQ(pixels(map), full);
    map.set_display(0, {}, core::Epoch{10}, core::Epoch{10});
    EXPECT_EQ(pixels(map), hidden);
    map.replace_track(0, track());
    EXPECT_EQ(pixels(map), full);
    map.set_selection(0);
    EXPECT_TRUE(map.tracks_ready()); // Selection only changes the moving marker overlay.
    EXPECT_NE(pixels(map), full);
    map.set_selection(std::nullopt);
    EXPECT_EQ(pixels(map), full);
}
TEST_F(GroundMapTest, ComposedMapMatchesDirectRenderingAfterLayerAndOverlayChanges) {
    GroundMap cached(imagery_);
    configure(cached);
    (void)pixels(cached);
    QThread::msleep(110); // The settled viewport starts composing after a zoom/drag burst.
    (void)pixels(cached);
    for (int stage = 0; stage < 8; ++stage) {
        GroundMap fresh(imagery_);
        configure(fresh);
        const auto change = [stage](GroundMap &map) {
            map.set_grid_visible(stage % 2 == 0);
            map.set_lighting(stage % 3 == 0);
            map.set_sun_visible(stage % 2 != 0);
            map.set_sun_direction(stage % 2 ? std::array<double, 3>{0, 1, 0}
                                            : std::array<double, 3>{1, 0, 0});
            map.set_labels_visible(stage % 3 != 0);
            map.set_selection(stage % 2 ? std::optional<std::size_t>(0) : std::nullopt);
            map.set_name(0, stage % 2 ? QStringLiteral("观测卫星") : QStringLiteral("Satellite"));
            map.set_snapshot({core::Epoch{stage},
                              core::FrameId::itrf,
                              core::OriginId::earth,
                              {{"Satellite", {7e6, stage * 1e6, 0}}, {"Station", {0, 7e6, 0}}}});
            map.set_access_link(std::pair<std::size_t, std::size_t>{0, 1}, stage % 2 == 0);
            DisplaySettings style;
            style.line_width = 1 + stage % 4;
            style.line_color = stage % 2 ? 0x73cdee : 0xff3040;
            map.set_display(0, style, core::Epoch{0}, core::Epoch{40});
            auto font = map.font();
            font.setPixelSize(12 + stage);
            map.setFont(font);
        };
        change(cached);
        change(fresh);
        const auto direct = pixels(fresh);
        expect_same_pixels(pixels(cached), direct);
        expect_same_pixels(pixels(cached), direct);
    }
}
TEST_F(GroundMapTest, CachedPanAndResizeMatchFreshRendering) {
    GroundMap cached(imagery_), fresh(imagery_);
    configure(cached);
    configure(fresh);
    for (auto *map : {&cached, &fresh})
        map->zoom(2, map->rect().center());
    (void)pixels(cached);
    pan(cached);
    pan(fresh);
    EXPECT_EQ(cached.marker_position(0), fresh.marker_position(0));
    expect_same_pixels(pixels(cached), pixels(fresh));
    cached.resize(880, 450);
    fresh.resize(880, 450);
    EXPECT_EQ(pixels(cached), pixels(fresh));
    cached.zoom(1.5, QPointF(200, 120));
    fresh.zoom(1.5, QPointF(200, 120));
    EXPECT_EQ(pixels(cached), pixels(fresh));
}
TEST_F(GroundMapTest, HiddenLightingAndSameEpochMarkerChangesAreNotStale) {
    GroundMap cached(imagery_), fresh(imagery_);
    configure(cached);
    configure(fresh);
    const auto original = pixels(cached);
    for (auto *map : {&cached, &fresh}) {
        map->set_lighting(false);
        map->set_sun_direction({0, 1, 0});
        map->set_lighting(true);
        map->set_snapshot({core::Epoch{10},
                           core::FrameId::itrf,
                           core::OriginId::earth,
                           {{"Satellite", {0, 7e6, 0}}}});
    }
    EXPECT_EQ(cached.epoch(), core::Epoch{10});
    EXPECT_NE(pixels(cached), original);
    EXPECT_EQ(pixels(cached), pixels(fresh));
}
TEST_F(GroundMapTest, PlaybackLightingAndDisabledSunLayersKeepImageryAndLatestDirection) {
    GroundMap cached(imagery_);
    configure(cached);
    const auto original = pixels(cached);
    QThread::msleep(110);
    (void)pixels(cached); // Reuse the settled foreground through subsequent sunlight changes.
    for (const auto &direction :
         {std::array<double, 3>{0, 0.8, 0.6}, std::array<double, 3>{0, 0, 1},
          std::array<double, 3>{-0.8, 0, -0.6}}) {
        GroundMap fresh(imagery_);
        configure(fresh);
        fresh.set_sun_direction(direction);
        cached.set_sun_direction(direction);
        EXPECT_NE(pixels(cached), original);
        expect_same_pixels(pixels(cached), pixels(fresh));
    }
    cached.set_lighting(false);
    cached.set_sun_visible(false);
    const auto unlit = pixels(cached);
    cached.set_sun_direction({0, -1, 0});
    EXPECT_EQ(pixels(cached), unlit);
    cached.set_lighting(true);
    cached.set_sun_visible(true);
    GroundMap fresh(imagery_);
    configure(fresh);
    fresh.set_sun_direction({0, -1, 0});
    expect_same_pixels(pixels(cached), pixels(fresh));
    for (auto *view : {&cached, &fresh}) {
        view->zoom(1.37, QPointF(220, 170));
        view->resize(731, 427);
        view->set_sun_direction({0.8, 0, 0.6});
    }
    expect_same_pixels(pixels(cached), pixels(fresh));
}
TEST_F(GroundMapTest, ContinuousLightingKeepsTheImageAndSettlesToTheLatestTime) {
    GroundMap map(imagery_), fresh(imagery_);
    configure(map);
    configure(fresh);
    const auto original = pixels(map);
    map.set_sun_direction({0, 1, 0}, true);
    // The first paint starts the worker and still draws the complete previous image.
    EXPECT_EQ(pixels(map), original);
    EXPECT_FALSE(map.background_ready());
    for (const auto direction : {std::array<double, 3>{0, 0, 1}, {-1, 0, 0}, {0, -0.8, -0.6}}) {
        map.set_sun_direction(direction, true);
        (void)pixels(map);
    }
    fresh.set_sun_direction({0, -0.8, -0.6});
    // No further time inputs: the pending/coalesced image must finish after pause.
    ASSERT_TRUE(settle(map));
    EXPECT_NE(pixels(map), original);
    expect_same_pixels(pixels(map), pixels(fresh));
}
TEST_F(GroundMapTest, PendingLightingCannotRestoreAnOldTimeSettingsOrViewport) {
    for (int change = 0; change < 3; ++change) {
        SCOPED_TRACE(change);
        GroundMap map(imagery_), fresh(imagery_);
        configure(map);
        configure(fresh);
        (void)pixels(map);
        map.set_sun_direction({0, 1, 0}, true);
        (void)pixels(map); // Leave an immutable job in flight.
        ASSERT_FALSE(map.background_ready());
        if (change == 0) {
            for (auto *view : {&map, &fresh})
                view->set_sun_direction({-0.8, 0, 0.6});
        } else if (change == 1) {
            for (auto *view : {&map, &fresh}) {
                view->set_lighting(false);
                view->set_sun_visible(false);
            }
        } else {
            fresh.set_sun_direction({0, 1, 0});
            for (auto *view : {&map, &fresh}) {
                view->zoom(1.37, QPointF(220, 170));
                view->resize(731, 427);
            }
        }
        expect_same_pixels(pixels(map), pixels(fresh));
        ASSERT_TRUE(settle(map)); // Also drain the superseded job before comparing again.
        expect_same_pixels(pixels(map), pixels(fresh));
    }
}
TEST_F(GroundMapTest, ClosingWithPendingLightingOwnsItsWorkerInputs) {
    GroundMap map(imagery_);
    configure(map);
    (void)pixels(map);
    map.set_sun_direction({0, 1, 0}, true);
    (void)pixels(map);
    EXPECT_FALSE(map.background_ready());
    // Destruction drains the worker before releasing images and timer state.
}
TEST_F(GroundMapTest, ObsoleteWorkerCannotRestoreAReplacedOrbit) {
    GroundMap map(imagery_), fresh(imagery_);
    configure(map);
    configure(fresh);
    map.replace_track(0, dense_track());
    (void)pixels(map); // Starts an immutable preview request.
    EXPECT_FALSE(map.tracks_ready());
    map.replace_track(0, track());
    ASSERT_TRUE(settle(map));
    EXPECT_EQ(pixels(map), pixels(fresh));
}
TEST_F(GroundMapTest, RefinementKeepsTheVisibleCurveUntilItsDetailedRasterIsReady) {
    GroundMap map(imagery_), fresh(imagery_);
    configure(map);
    configure(fresh);
    const auto before = pixels(map);
    ASSERT_TRUE(map.tracks_ready());
    const auto detailed = dense_track();
    map.refine_tracks({detailed});
    EXPECT_TRUE(map.tracks_transitioning());
    // The first paint starts the asynchronous raster, without an empty frame or a
    // lower-quality whole-world raster being shown in between.
    EXPECT_EQ(pixels(map), before);
    EXPECT_FALSE(map.tracks_ready());
    fresh.publish_tracks({detailed});
    ASSERT_TRUE(settle(map));
    ASSERT_TRUE(settle(fresh));
    EXPECT_FALSE(map.tracks_transitioning());
    expect_same_pixels(pixels(map), pixels(fresh));
}
TEST_F(GroundMapTest, RefinementBlendsWithoutDimmingAndNewInputInterruptsIt) {
    GroundMap map(imagery_), fresh(imagery_);
    configure(map);
    configure(fresh);
    const auto before = pixels(map);
    // Identical geometry must keep constant brightness even halfway through a fade.
    map.refine_tracks({track()});
    EXPECT_EQ(pixels(map), before);
    ASSERT_TRUE(map.tracks_transitioning());
    auto *animation = map.findChild<QVariantAnimation *>("groundMapRefinement");
    ASSERT_NE(animation, nullptr);
    animation->setCurrentTime(animation->duration() / 2);
    {
        SCOPED_TRACE("constant brightness at halfway");
        expect_same_pixels(pixels(map), before);
    }
    auto changed = track();
    changed.points[1].ry() += 2;
    changed.path = ground_path(changed.points);
    animation->setCurrentTime(animation->duration());
    map.refine_tracks({changed});
    EXPECT_EQ(pixels(map), before);
    animation->setCurrentTime(animation->duration() / 2);
    const auto midway = pixels(map);
    EXPECT_NE(midway, before);
    fresh.publish_tracks({changed});
    EXPECT_NE(midway, pixels(fresh));
    animation->setCurrentTime(animation->duration());
    {
        SCOPED_TRACE("exact detail after transition");
        expect_same_pixels(pixels(map), pixels(fresh));
    }

    map.refine_tracks({track()});
    (void)pixels(map);
    ASSERT_TRUE(map.tracks_transitioning());
    map.publish_tracks({});
    fresh.publish_tracks({});
    EXPECT_FALSE(map.tracks_transitioning());
    expect_same_pixels(pixels(map), pixels(fresh));
    ASSERT_TRUE(settle(map));
    expect_same_pixels(pixels(map), pixels(fresh));

    map.publish_tracks({track()});
    (void)pixels(map);
    map.refine_tracks({dense_track()});
    (void)pixels(map);
    map.publish_tracks({changed}); // Interrupt a pending worker as well as a fade.
    fresh.publish_tracks({changed});
    ASSERT_TRUE(settle(map));
    expect_same_pixels(pixels(map), pixels(fresh));

    map.refine_tracks({track()});
    (void)pixels(map);
    ASSERT_TRUE(map.tracks_transitioning());
    map.zoom_centered(2);
    fresh.publish_tracks({track()});
    fresh.zoom_centered(2);
    ASSERT_TRUE(settle(map));
    EXPECT_FALSE(map.tracks_transitioning());
    expect_same_pixels(pixels(map), pixels(fresh));
}
TEST_F(GroundMapTest, ZoomBurstAndResizeSettleAtTheLatestViewport) {
    GroundMap map(imagery_), fresh(imagery_);
    for (auto *view : {&map, &fresh}) {
        configure(*view);
        view->replace_track(0, dense_track());
    }
    ASSERT_TRUE(settle(map));
    for (int i = 0; i < 12; ++i) {
        for (auto *view : {&map, &fresh})
            view->zoom(i < 8 ? 1.2 : 1 / 1.2, QPointF(240, 180));
        EXPECT_FALSE(map.tracks_ready());
        (void)pixels(map); // Reproject cached coverage while obsolete work is cancelled.
        QCoreApplication::processEvents();
    }
    for (auto *view : {&map, &fresh}) {
        view->resize(820, 480);
        view->set_selection(0);
    }
    ASSERT_TRUE(settle(map));
    ASSERT_TRUE(settle(fresh));
    expect_same_pixels(pixels(map), pixels(fresh));
}
TEST_F(GroundMapTest, PanBeyondCachedCoverageAndBackgroundChangesMatchFreshRendering) {
    GroundMap cached(imagery_), fresh(imagery_);
    for (auto *map : {&cached, &fresh}) {
        configure(*map);
        map->zoom(4, QPointF(300, 180));
    }
    (void)pixels(cached);
    for (int i = 0; i < 12; ++i) {
        pan(cached);
        pan(fresh);
        (void)pixels(cached);
    }
    expect_same_pixels(pixels(cached), pixels(fresh));
    for (auto *map : {&cached, &fresh}) {
        map->set_sun_direction({0, 0.8, 0.6});
        map->set_grid_visible(false);
        map->set_sun_visible(false);
    }
    expect_same_pixels(pixels(cached), pixels(fresh));
}
TEST_F(GroundMapTest, WheelZoomRespondsImmediatelyAndKeepsThePointUnderTheCursor) {
    GroundMap map(imagery_);
    configure(map);
    (void)pixels(map);
    map.zoom(2, QPointF(250, 150));
    const auto anchor = map.marker_position(0);
    ASSERT_TRUE(wheel(map, anchor, {0, 120}));
    EXPECT_GT(map.zoom_factor(), 2);
    EXPECT_LT(map.zoom_factor(), 2.4);
    ASSERT_TRUE(wait_zoom(map, 2.4));
    EXPECT_NEAR(QLineF(map.marker_position(0), anchor).length(), 0, 1e-9);
    // A second gesture after a completed animation must not reuse its old end value.
    wheel(map, anchor, {0, 60});
    EXPECT_GT(map.zoom_factor(), 2.4);
    ASSERT_TRUE(wait_zoom(map, 2.4 * std::sqrt(1.2)));
    EXPECT_NEAR(QLineF(map.marker_position(0), anchor).length(), 0, 1e-9);
}
TEST_F(GroundMapTest, PixelScrollingIsContinuousAndDoesNotLeaveAQueuedAnimation) {
    GroundMap map(imagery_);
    configure(map);
    (void)pixels(map);
    map.zoom(2, QPointF(250, 150));
    const auto anchor = map.marker_position(0);
    wheel(map, anchor, {0, 120});
    const auto before = map.zoom_factor();
    ASSERT_TRUE(wheel(map, anchor, {0, 120}, {0, 3}));
    EXPECT_NEAR(map.zoom_factor(), before * std::pow(1.2, 3.0 / 60), 1e-9);
    const auto continuous = map.zoom_factor();
    process_for(170);
    EXPECT_DOUBLE_EQ(map.zoom_factor(), continuous);
    wheel(map, anchor, {}, {0, -3});
    EXPECT_NEAR(map.zoom_factor(), before, 1e-9);
    EXPECT_NEAR(QLineF(map.marker_position(0), anchor).length(), 0, 1e-9);
}
TEST_F(GroundMapTest, WheelBurstsAccumulateAndReverseFromTheVisibleScale) {
    GroundMap map(imagery_);
    configure(map);
    (void)pixels(map);
    map.zoom(2, map.marker_position(0));
    const auto anchor = map.marker_position(0);
    for (int i = 0; i < 4; ++i)
        wheel(map, anchor, {0, 120});
    ASSERT_TRUE(wait_zoom(map, 2 * std::pow(1.2, 4)));
    wheel(map, anchor, {0, 120});
    wheel(map, anchor, {0, 120});
    const auto before_reverse = map.zoom_factor();
    wheel(map, anchor, {0, -120});
    EXPECT_LT(map.zoom_factor(), before_reverse);
    ASSERT_TRUE(wait_zoom(map, before_reverse / 1.2));
}
TEST_F(GroundMapTest, InvalidZoomAndWheelOutsideTheMapDoNotChangeTheViewport) {
    GroundMap map(imagery_);
    configure(map);
    (void)pixels(map);
    const auto anchor = map.marker_position(0);
    EXPECT_FALSE(wheel(map, {10, 10}, {0, 120}));
    EXPECT_FALSE(wheel(map, {320, 8}, {0, 120})); // Outside the plotting area.
    EXPECT_FALSE(wheel(map, anchor, {120, 0}));
    EXPECT_FALSE(wheel(map, anchor, {}));
    for (double factor : {0.0, -1.0, std::numeric_limits<double>::infinity(),
                          std::numeric_limits<double>::quiet_NaN()})
        map.zoom(factor, anchor);
    map.zoom(2, {std::numeric_limits<double>::quiet_NaN(), 0});
    process_for(170);
    EXPECT_EQ(map.zoom_factor(), 1);
    EXPECT_EQ(map.marker_position(0), anchor);
    map.zoom(10000, anchor);
    EXPECT_EQ(map.zoom_factor(), 1024);
    wheel(map, anchor, {0, 120});
    EXPECT_EQ(map.zoom_factor(), 1024);
    map.zoom(0.0001, anchor);
    const auto minimum = map.zoom_factor();
    EXPECT_LT(minimum, 1);
    EXPECT_GT(minimum, 0);
    wheel(map, anchor, {0, -120});
    EXPECT_EQ(map.zoom_factor(), minimum);
    EXPECT_FALSE(wheel(map, {320, 20}, {0, 120})); // Letterboxing at the whole-world scale.
}
TEST_F(GroundMapTest, DragKeepsTheFullDisplacementAndReleasePositionWithoutSelecting) {
    GroundMap map(imagery_);
    configure(map);
    (void)pixels(map);
    map.zoom(3, map.marker_position(0));
    const auto origin = map.marker_position(0);
    int selected = 0;
    map.select = [&](auto) { ++selected; };
    mouse(map, QEvent::MouseButtonPress, origin);
    mouse(map, QEvent::MouseMove, origin + QPointF(1, 0));
    EXPECT_EQ(map.marker_position(0), origin);
    mouse(map, QEvent::MouseMove, origin + QPointF(40, 20));
    EXPECT_NEAR(QLineF(map.marker_position(0), origin + QPointF(40, 20)).length(), 0, 1e-9);
    mouse(map, QEvent::MouseButtonRelease, origin + QPointF(50, 30));
    EXPECT_NEAR(QLineF(map.marker_position(0), origin + QPointF(50, 30)).length(), 0, 1e-9);
    EXPECT_EQ(selected, 0);
}
TEST_F(GroundMapTest, MiddleButtonPansAndLeftClickStillSelectsWithSmallJitter) {
    GroundMap map(imagery_);
    configure(map);
    (void)pixels(map);
    map.zoom(3, map.marker_position(0));
    int calls = 0;
    std::optional<std::size_t> selected;
    map.select = [&](auto index) {
        ++calls;
        selected = index;
    };
    auto point = map.marker_position(0);
    mouse(map, QEvent::MouseButtonPress, point, Qt::MiddleButton);
    mouse(map, QEvent::MouseButtonRelease, point, Qt::MiddleButton);
    EXPECT_EQ(calls, 0);
    mouse(map, QEvent::MouseButtonPress, point, Qt::MiddleButton);
    mouse(map, QEvent::MouseMove, point + QPointF(40, 0), Qt::MiddleButton);
    mouse(map, QEvent::MouseButtonRelease, point + QPointF(40, 0), Qt::MiddleButton);
    EXPECT_EQ(calls, 0);
    EXPECT_NEAR(map.marker_position(0).x(), point.x() + 40, 1e-9);
    point = map.marker_position(0);
    mouse(map, QEvent::MouseButtonPress, point);
    mouse(map, QEvent::MouseMove, point + QPointF(1, 1));
    mouse(map, QEvent::MouseButtonRelease, point + QPointF(1, 1));
    EXPECT_EQ(calls, 1);
    ASSERT_TRUE(selected);
    EXPECT_EQ(*selected, 0u);
    EXPECT_EQ(map.marker_position(0), point);
    mouse(map, QEvent::MouseButtonPress, {10, 10});
    mouse(map, QEvent::MouseButtonRelease, {10, 10});
    EXPECT_EQ(calls, 1);
}
TEST_F(GroundMapTest, EscapeRestoresDragAndFocusLossEndsItWithoutLateMovement) {
    GroundMap map(imagery_);
    configure(map);
    (void)pixels(map);
    map.zoom(3, map.marker_position(0));
    const auto origin = map.marker_position(0);
    mouse(map, QEvent::MouseButtonPress, origin);
    mouse(map, QEvent::MouseMove, origin + QPointF(80, 30));
    key(map, Qt::Key_Escape);
    EXPECT_EQ(map.marker_position(0), origin);
    mouse(map, QEvent::MouseButtonRelease, origin + QPointF(80, 30));
    EXPECT_EQ(map.marker_position(0), origin);
    mouse(map, QEvent::MouseButtonPress, origin, Qt::MiddleButton);
    mouse(map, QEvent::MouseMove, origin + QPointF(40, 10), Qt::MiddleButton);
    QFocusEvent focus_out(QEvent::FocusOut);
    QCoreApplication::sendEvent(&map, &focus_out);
    const auto stopped = map.marker_position(0);
    mouse(map, QEvent::MouseMove, origin + QPointF(100, 60), Qt::MiddleButton);
    EXPECT_EQ(map.marker_position(0), stopped);
    EXPECT_NE(map.cursor().shape(), Qt::ClosedHandCursor);
}
TEST_F(GroundMapTest, LatitudeEdgesClampWithoutADeadZoneWhenTheDragReverses) {
    GroundMap map(imagery_);
    configure(map);
    (void)pixels(map);
    map.zoom(2, map.marker_position(0));
    const auto origin = map.marker_position(0);
    mouse(map, QEvent::MouseButtonPress, origin);
    mouse(map, QEvent::MouseMove, origin + QPointF(0, 5000));
    const auto edge = map.marker_position(0);
    EXPECT_LT(edge.y(), origin.y() + 1000);
    mouse(map, QEvent::MouseMove, origin + QPointF(0, 4990));
    EXPECT_NEAR(map.marker_position(0).y(), edge.y() - 10, 1e-9);
    key(map, Qt::Key_Escape);
    EXPECT_EQ(map.marker_position(0), origin);
}
TEST_F(GroundMapTest, LongitudePansThroughMultipleWorldsInBothDirectionsAndEscapeRestoresIt) {
    GroundMap map(imagery_);
    configure(map);
    map.set_snapshot({core::Epoch{10},
                      core::FrameId::itrf,
                      core::OriginId::earth,
                      {{"Origin", position(0, 0)}, {"Quarter world", position(90, 0)}}});
    (void)pixels(map);
    map.zoom(3, map.marker_position(0));
    const auto origin = map.marker_position(0);
    const auto period = (map.marker_position(1).x() - origin.x()) * 4;
    ASSERT_GT(period, map.width());
    for (const auto turns : {3, -7, 100, -100}) {
        SCOPED_TRACE(turns);
        drag(map, {turns * period + 37, 0});
        EXPECT_NEAR(map.marker_position(0).x(), origin.x() + 37, 1e-7);
        EXPECT_NEAR(map.marker_position(0).y(), origin.y(), 1e-9);
        EXPECT_NEAR(map.marker_coordinates(0).x(), 0, 1e-10);
        drag(map, {-37, 0});
        EXPECT_NEAR(QLineF(map.marker_position(0), origin).length(), 0, 1e-7);
    }
    mouse(map, QEvent::MouseButtonPress, origin, Qt::MiddleButton);
    mouse(map, QEvent::MouseMove, origin + QPointF(4 * period + 60, 0), Qt::MiddleButton);
    EXPECT_NEAR(map.marker_position(0).x(), origin.x() + 60, 1e-7);
    mouse(map, QEvent::MouseMove, origin + QPointF(4 * period + 50, 0), Qt::MiddleButton);
    EXPECT_NEAR(map.marker_position(0).x(), origin.x() + 50, 1e-7);
    key(map, Qt::Key_Escape);
    EXPECT_NEAR(QLineF(map.marker_position(0), origin).length(), 0, 1e-7);
}
TEST_F(GroundMapTest, DateLineNeighborsRemainAdjacentAndEveryVisibleWorldCopyCanBePicked) {
    GroundMap map(imagery_);
    configure(map);
    map.set_snapshot({core::Epoch{10},
                      core::FrameId::itrf,
                      core::OriginId::earth,
                      {{"East", position(179, 0)}, {"West", position(-179, 0)}}});
    map.set_selection(0);
    map.focus_selection();
    map.zoom(2, map.marker_position(0));
    const auto east = map.marker_position(0), west = map.marker_position(1);
    EXPECT_GT(west.x(), east.x());
    EXPECT_LT(QLineF(east, west).length(), 20);
    drag(map, {-30, 0}); // Cross the antimeridian while keeping both objects visible.
    EXPECT_NEAR(map.marker_position(0).x(), east.x() - 30, 1e-8);
    EXPECT_NEAR(map.marker_position(1).x(), west.x() - 30, 1e-8);
    EXPECT_NEAR(map.marker_coordinates(0).x(), 179, 1e-9);
    EXPECT_NEAR(map.marker_coordinates(1).x(), -179, 1e-9);
    std::optional<std::size_t> selected;
    int calls = 0;
    map.select = [&](auto index) {
        selected = index;
        ++calls;
    };
    for (std::size_t index = 0; index < 2; ++index) {
        mouse(map, QEvent::MouseButtonPress, map.marker_position(index));
        mouse(map, QEvent::MouseButtonRelease, map.marker_position(index));
        ASSERT_TRUE(selected);
        EXPECT_EQ(*selected, index);
    }
    EXPECT_EQ(calls, 2);

    map.resize(1000, 160);
    map.reset_view();
    map.set_snapshot({core::Epoch{10},
                      core::FrameId::itrf,
                      core::OriginId::earth,
                      {{"Origin", position(0, 0)}, {"Quarter world", position(90, 0)}}});
    const auto plot = QRectF(map.rect()).adjusted(38, 14, -16, -44);
    map.zoom(0.0001, plot.center());
    const auto origin = map.marker_position(0);
    const auto period = (map.marker_position(1).x() - origin.x()) * 4;
    ASSERT_GT(period, 0);
    EXPECT_NEAR(QLineF(origin, plot.center()).length(), 0, 1e-8);
    calls = 0;
    for (int copy = -2; copy <= 2; ++copy) {
        const auto point = origin + QPointF(copy * period, 0);
        ASSERT_TRUE(plot.contains(point));
        mouse(map, QEvent::MouseButtonPress, point);
        mouse(map, QEvent::MouseButtonRelease, point);
        ASSERT_TRUE(selected);
        EXPECT_EQ(*selected, 0u) << "world copy " << copy;
    }
    EXPECT_EQ(calls, 5);
    EXPECT_TRUE(wheel(map, origin + QPointF(period, 0), {0, 120}));
}
TEST_F(GroundMapTest, WrappedTracksFootprintsAndLightingMatchFreshRenderingAcrossTheSeam) {
    QImage imagery(720, 360, QImage::Format_RGB32);
    imagery.fill(QColor(90, 110, 130));
    GroundMap cached(imagery), fresh(imagery);
    auto seam_track = track();
    seam_track.points = {{170, 0}, {-170, 0}};
    seam_track.epochs = {core::Epoch{0}, core::Epoch{10}};
    seam_track.last = 2;
    seam_track.path = ground_path(seam_track.points);
    seam_track.display.line_color = 0xff3040;
    seam_track.display.line_width = 3;
    visualization::SensorMesh mesh;
    mesh.ground = {{position(175, -8), position(-175, -8), position(180, 8)}};
    mesh.ground_edges = {{position(175, -8), position(-175, -8)},
                         {position(-175, -8), position(180, 8)},
                         {position(180, 8), position(175, -8)}};
    mesh.color = 0x00ddff;
    mesh.opacity = .4;
    for (auto *map : {&cached, &fresh}) {
        configure(*map);
        map->publish_tracks({seam_track});
        map->set_snapshot({core::Epoch{10},
                           core::FrameId::itrf,
                           core::OriginId::earth,
                           {{"Seam", position(180, 0)}}});
        map->set_selection(0);
        map->focus_selection();
        map->zoom(2, map->marker_position(0));
        map->set_grid_visible(false);
        map->set_labels_visible(false);
        map->set_sun_visible(false);
        map->set_sun_direction({0, 1, 0}); // Twilight itself crosses the visible seam.
    }
    ASSERT_TRUE(settle(cached));
    const auto without_footprint = pixels(cached);
    const auto center = cached.marker_position(0);
    for (int offset : {-35, -20, 20, 35}) {
        const auto point = ((center + QPointF(offset, 0)) * cached.devicePixelRatioF()).toPoint();
        const auto color = without_footprint.pixelColor(point);
        EXPECT_GT(color.red(), 180) << offset;
        EXPECT_LT(color.green(), 100) << offset;
    }
    const auto daylight = ((center + QPointF(-45, 60)) * cached.devicePixelRatioF()).toPoint();
    const auto night = ((center + QPointF(45, 60)) * cached.devicePixelRatioF()).toPoint();
    EXPECT_GT(without_footprint.pixelColor(daylight).blue(),
              without_footprint.pixelColor(night).blue() + 30);
    for (auto *map : {&cached, &fresh})
        map->set_sensor_footprints(std::span(&mesh, 1));
    ASSERT_TRUE(settle(cached));
    const auto with_footprint = pixels(cached);
    for (int offset : {-20, 20}) {
        const auto point = ((center + QPointF(offset, 20)) * cached.devicePixelRatioF()).toPoint();
        EXPECT_GT(with_footprint.pixelColor(point).blue(),
                  without_footprint.pixelColor(point).blue());
    }
    // The cached view crosses the seam and returns through several world copies;
    // the reference is first rasterized directly at the same final longitude.
    const auto plot = QRectF(cached.rect()).adjusted(38, 14, -16, -44);
    const auto period = std::max(plot.width(), 2 * plot.height()) * cached.zoom_factor();
    for (const auto delta :
         {QPointF(35, 0), QPointF(2 * period, 0), QPointF(-3 * period + 22.25, 10.5)}) {
        drag(cached, delta);
        (void)pixels(cached);
        drag(fresh, delta);
    }
    ASSERT_TRUE(settle(cached));
    ASSERT_TRUE(settle(fresh));
    expect_same_pixels(pixels(cached), pixels(fresh));
    const auto settled = pixels(cached);
    drag(cached, {period, 0});
    ASSERT_TRUE(settle(cached));
    expect_same_pixels(pixels(cached), settled);
    capture(pixels(cached), "seam");
}
TEST_F(GroundMapTest, DenseTrackPreviewRepeatsAcrossTheDateLineDuringAnUnfinishedDrag) {
    GroundMap map(imagery_);
    configure(map);
    auto dense = track();
    dense.points.clear();
    dense.epochs.clear();
    for (int index = 0; index < 4000; ++index) {
        dense.points.emplace_back(std::remainder(160 + index * .7, 360.0), 0);
        dense.epochs.emplace_back(index);
    }
    dense.last = dense.points.size();
    dense.path = ground_path(dense.points);
    dense.display.line_color = 0xff3040;
    dense.display.line_width = 3;
    map.publish_tracks({dense});
    map.set_labels_visible(false);
    map.set_grid_visible(false);
    map.set_lighting(false);
    map.set_sun_visible(false);
    const auto plot = QRectF(map.rect()).adjusted(38, 14, -16, -44);
    map.zoom(4, plot.center());
    // Settling the initial longitude prepares both the bounded world preview and
    // an exact viewport raster. The seam is outside that raster's overscan.
    ASSERT_TRUE(settle(map));
    const auto period = std::max(plot.width(), 2 * plot.height()) * map.zoom_factor();
    const auto origin = plot.center();
    mouse(map, QEvent::MouseButtonPress, origin, Qt::MiddleButton);
    QPointF displacement;
    for (const auto turns : {0.5, 2.5, -3.5}) {
        SCOPED_TRACE(turns);
        displacement = {turns * period, 0};
        mouse(map, QEvent::MouseMove, origin + displacement, Qt::MiddleButton);
        const auto preview = pixels(map);
        // The first jump must show the repeated preview before detail is ready.
        // Later turns reach the same canonical longitude; the useful background
        // request may finish while the button remains held.
        if (turns == .5)
            ASSERT_FALSE(map.tracks_ready());
        for (const auto offset : {-100, -40, 40, 100}) {
            bool visible = false;
            const auto point = ((origin + QPointF(offset, 0)) * map.devicePixelRatioF()).toPoint();
            for (int dy = -qCeil(3 * map.devicePixelRatioF());
                 dy <= qCeil(3 * map.devicePixelRatioF()); ++dy) {
                const auto color = preview.pixelColor(point + QPoint(0, dy));
                visible |= color.red() > 180 && color.green() < 100;
            }
            EXPECT_TRUE(visible) << "seam offset " << offset;
        }
        capture(preview, turns == .5 ? "preview-first-seam" : "preview-wrapped-seam");
    }
    mouse(map, QEvent::MouseButtonRelease, origin + displacement, Qt::MiddleButton);
    ASSERT_TRUE(settle(map));
}
TEST_F(GroundMapTest, FractionalPanPreservesCachedTextureAndThinTracksWithoutQuantizingInput) {
    QImage imagery(720, 360, QImage::Format_RGB32);
    for (int y = 0; y < imagery.height(); ++y)
        for (int x = 0; x < imagery.width(); ++x)
            imagery.setPixel(x, y, (x + y) % 2 ? qRgb(230, 230, 230) : qRgb(25, 25, 25));
    GroundMap map(imagery);
    configure(map);
    auto dense = dense_track();
    for (auto &point : dense.points)
        point.setY(0);
    dense.path = ground_path(dense.points);
    dense.display.line_color = 0xff3040;
    dense.display.line_width = 1;
    map.publish_tracks({dense});
    map.set_labels_visible(false);
    map.set_grid_visible(false);
    map.set_lighting(false);
    map.set_sun_visible(false);
    const auto plot = QRectF(map.rect()).adjusted(38, 14, -16, -44);
    map.zoom(2, plot.center());
    ASSERT_TRUE(settle(map));
    const auto before = pixels(map);
    const auto marker = map.marker_position(0);
    const auto ratio = map.devicePixelRatioF();
    const QRect region(qRound(100 * ratio), qRound(80 * ratio), qRound(150 * ratio),
                       qRound(200 * ratio));
    mouse(map, QEvent::MouseButtonPress, plot.center(), Qt::MiddleButton);
    QPointF displacement;
    for (int step = 1; step <= 16; ++step) {
        SCOPED_TRACE(step);
        displacement = {12 + step * .17 / ratio, 1 + step * .11 / ratio};
        mouse(map, QEvent::MouseMove, plot.center() + displacement, Qt::MiddleButton);
        EXPECT_NEAR(QLineF(map.marker_position(0), marker + displacement).length(), 0, 1e-8);
        const auto navigating = pixels(map);
        const QPoint device_offset(qRound(displacement.x() * ratio),
                                   qRound(displacement.y() * ratio));
        // A pure pan copies the already filtered texture and track pixels once.
        // Nearest-device-pixel placement bounds its visual offset by half a pixel
        // without softening the entire frame or rounding geographic interaction.
        expect_same_pixels(navigating.copy(region), before.copy(region.translated(-device_offset)));
    }
    capture(pixels(map), "fractional-pan-sharp");
    // Pausing within a drag restores the exact fractional sampling phase too;
    // users should not have to release the button to finish this refinement.
    ASSERT_EQ(map.cursor().shape(), Qt::ClosedHandCursor);
    ASSERT_TRUE(settle(map));
    ASSERT_EQ(map.cursor().shape(), Qt::ClosedHandCursor);
    GroundMap fresh(imagery);
    configure(fresh);
    fresh.publish_tracks({dense});
    fresh.set_labels_visible(false);
    fresh.set_grid_visible(false);
    fresh.set_lighting(false);
    fresh.set_sun_visible(false);
    fresh.zoom(2, plot.center());
    drag(fresh, displacement);
    ASSERT_TRUE(settle(fresh));
    EXPECT_NEAR(QLineF(map.marker_position(0), fresh.marker_position(0)).length(), 0, 1e-8);
    expect_same_pixels(pixels(map).copy(region), pixels(fresh).copy(region));
    capture(pixels(map), "fractional-pan-settled-button-held");
    key(map, Qt::Key_Escape);
    EXPECT_NEAR(QLineF(map.marker_position(0), marker).length(), 0, 1e-8);
    ASSERT_TRUE(settle(map));
    expect_same_pixels(pixels(map).copy(region), before.copy(region));
}
TEST_F(GroundMapTest, PanBeyondOverscanKeepsVisibleTrackDetailAndRefillsWhileButtonIsHeld) {
    QImage imagery(720, 360, QImage::Format_RGB32);
    imagery.fill(QColor(90, 110, 130));
    GroundMap map(imagery);
    configure(map);
    auto dense = dense_track();
    for (auto &point : dense.points)
        point.setY(0);
    dense.path = ground_path(dense.points);
    dense.display.line_color = 0xff3040;
    dense.display.line_width = 1;
    map.publish_tracks({dense});
    map.set_snapshot({core::Epoch{10}, core::FrameId::itrf, core::OriginId::earth, {}});
    map.set_labels_visible(false);
    map.set_grid_visible(false);
    map.set_lighting(false);
    map.set_sun_visible(false);
    const auto plot = QRectF(map.rect()).adjusted(38, 14, -16, -44);
    map.zoom(4, plot.center());
    ASSERT_TRUE(settle(map));
    const auto before = pixels(map);
    const auto ratio = map.devicePixelRatioF();
    const QPointF displacement(240, 0);
    mouse(map, QEvent::MouseButtonPress, plot.center(), Qt::MiddleButton);
    mouse(map, QEvent::MouseMove, plot.center() + displacement, Qt::MiddleButton);
    const auto navigating = pixels(map);
    ASSERT_FALSE(map.tracks_ready());
    const QRect retained(qRound(300 * ratio), qRound((plot.center().y() - 12) * ratio),
                         qRound(200 * ratio), qRound(24 * ratio));
    // The left edge is newly exposed, but the rest still has exact viewport
    // detail. Switching that retained region to the world preview widens lines.
    expect_same_pixels(navigating.copy(retained),
                       before.copy(retained.translated(-qRound(displacement.x() * ratio), 0)));
    capture(navigating, "retained-detail-during-pan");
    ASSERT_EQ(map.cursor().shape(), Qt::ClosedHandCursor);
    ASSERT_TRUE(settle(map));
    ASSERT_EQ(map.cursor().shape(), Qt::ClosedHandCursor);
    const auto detailed = pixels(map);
    for (const int x : {65, 100, 300, 500}) {
        bool visible = false;
        const auto center = (QPointF(x, plot.center().y()) * ratio).toPoint();
        for (int dy = -qCeil(2 * ratio); dy <= qCeil(2 * ratio); ++dy) {
            const auto color = detailed.pixelColor(center + QPoint(0, dy));
            visible |= color.red() > 180 && color.green() < 100;
        }
        EXPECT_TRUE(visible) << "detail at x=" << x;
    }
    expect_same_pixels(detailed.copy(retained), navigating.copy(retained));
    capture(detailed, "refilled-detail-button-held");
    mouse(map, QEvent::MouseButtonRelease, plot.center() + displacement, Qt::MiddleButton);
}
TEST_F(GroundMapTest, PartialSensorDetailAndPreviewApplyOpacityOnceAcrossTheirBoundary) {
    QImage imagery(720, 360, QImage::Format_RGB32);
    imagery.fill(QColor(90, 110, 130));
    GroundMap map(imagery);
    configure(map);
    map.publish_tracks({});
    map.set_snapshot({core::Epoch{10}, core::FrameId::itrf, core::OriginId::earth, {}});
    map.set_labels_visible(false);
    map.set_grid_visible(false);
    map.set_lighting(false);
    map.set_sun_visible(false);
    const auto plot = QRectF(map.rect()).adjusted(38, 14, -16, -44);
    map.zoom(4, plot.center());
    const auto unfilled = pixels(map);
    visualization::SensorMesh mesh;
    mesh.ground = {{position(-80, -50), position(80, -50), position(80, 50)},
                   {position(-80, -50), position(80, 50), position(-80, 50)}};
    mesh.color = 0x00ddff;
    mesh.opacity = .4;
    map.set_sensor_footprints(std::span(&mesh, 1));
    ASSERT_TRUE(settle(map));
    const auto ratio = map.devicePixelRatioF();
    const auto center = (plot.center() * ratio).toPoint();
    const auto coverage = pixels(map).pixelColor(center);
    ASSERT_GT(coverage.blue(), unfilled.pixelColor(center).blue());
    const QPointF displacement(240, 0);
    mouse(map, QEvent::MouseButtonPress, plot.center(), Qt::MiddleButton);
    mouse(map, QEvent::MouseMove, plot.center() + displacement, Qt::MiddleButton);
    const auto navigating = pixels(map);
    ASSERT_FALSE(map.footprints_ready());
    // Include both sides of the old overscan edge (38 - 160 + 240 = 118).
    // The new strip uses preview coverage, while the old strip keeps detail;
    // compositing both at once would visibly darken the overlap.
    for (int x = 65; x <= 500; ++x)
        EXPECT_EQ(navigating.pixelColor((QPointF(x, plot.center().y()) * ratio).toPoint()),
                  coverage)
            << "coverage at x=" << x;
    capture(navigating, "sensor-detail-preview-boundary");
    ASSERT_TRUE(settle(map));
    const auto detailed = pixels(map);
    const QRect interior(qRound(65 * ratio), qRound(80 * ratio), qRound(435 * ratio),
                         qRound(200 * ratio));
    expect_same_pixels(detailed.copy(interior), navigating.copy(interior));
    mouse(map, QEvent::MouseButtonRelease, plot.center() + displacement, Qt::MiddleButton);
}
TEST_F(GroundMapTest, HighZoomDragKeepsDenseTracksThinAndPublishesTheCurrentViewport) {
    QImage imagery(720, 360, QImage::Format_RGB32);
    imagery.fill(QColor(90, 110, 130));
    GroundMap map(imagery);
    configure(map);
    map.publish_tracks({});
    map.set_snapshot({core::Epoch{10},
                      core::FrameId::itrf,
                      core::OriginId::earth,
                      {{"Nearby", position(0, .05)}}});
    map.set_labels_visible(false);
    map.set_grid_visible(false);
    map.set_lighting(false);
    map.set_sun_visible(false);
    const auto plot = QRectF(map.rect()).adjusted(38, 14, -16, -44);
    map.zoom(1024, plot.center());
    const auto background = pixels(map);
    auto dense = dense_track();
    for (auto &point : dense.points)
        point.setY(0);
    dense.path = ground_path(dense.points);
    dense.display.line_color = 0xff3040;
    dense.display.line_width = 3;
    map.publish_tracks({dense});
    ASSERT_TRUE(settle(map));

    const auto origin = plot.center();
    const auto ratio = map.devicePixelRatioF();
    mouse(map, QEvent::MouseButtonPress, origin, Qt::MiddleButton);
    QPointF displacement;
    for (const auto delta : {QPointF(240, 25), QPointF(480, -20)}) {
        displacement = delta;
        mouse(map, QEvent::MouseMove, origin + displacement, Qt::MiddleButton);
        const auto navigating = pixels(map);
        // Leave the exact raster's overscan, then move again before its worker
        // publishes. A low-resolution world preview must never become a broad
        // band across the viewport at this scale.
        for (const int offset : {-60, 60}) {
            const auto point = ((origin + QPointF(0, displacement.y() + offset)) * ratio).toPoint();
            EXPECT_EQ(navigating.pixelColor(point), background.pixelColor(point));
        }
    }
    // The button is still held. Allow asynchronous detail to catch up during a
    // pause in this same drag; requiring release would leave navigation blank.
    ASSERT_EQ(map.cursor().shape(), Qt::ClosedHandCursor);
    ASSERT_TRUE(settle(map));
    ASSERT_EQ(map.cursor().shape(), Qt::ClosedHandCursor);
    const auto current = pixels(map);
    for (const int offset : {-100, 0, 100}) {
        const auto point = ((origin + QPointF(offset, displacement.y())) * ratio).toPoint();
        const auto color = current.pixelColor(point);
        EXPECT_GT(color.red(), 180);
        EXPECT_LT(color.green(), 100);
    }
    // A completed request from the earlier pointer position must not restore its
    // old latitude when the current viewport's result is published.
    for (const int offset : {25, -80, 40}) {
        const auto point = ((origin + QPointF(0, offset)) * ratio).toPoint();
        EXPECT_EQ(current.pixelColor(point), background.pixelColor(point));
    }
    capture(current, "1024x-active-drag");
    mouse(map, QEvent::MouseButtonRelease, origin + displacement, Qt::MiddleButton);
    ASSERT_TRUE(settle(map));
    expect_same_pixels(pixels(map), current);
}
TEST_F(GroundMapTest, HighZoomClipsWorldLengthTracksAndSensorFacetsToTheViewport) {
    QImage imagery(720, 360, QImage::Format_RGB32);
    imagery.fill(QColor(90, 110, 130));
    GroundMap map(imagery);
    configure(map);
    auto long_track = track();
    long_track.points = {{-80, 0}, {80, 0}};
    long_track.epochs = {core::Epoch{0}, core::Epoch{10}};
    long_track.last = 2;
    long_track.path = ground_path(long_track.points);
    long_track.display.line_color = 0xff3040;
    long_track.display.line_width = 3;
    map.publish_tracks({long_track});
    map.set_snapshot({core::Epoch{10},
                      core::FrameId::itrf,
                      core::OriginId::earth,
                      {{"Nearby", position(0, .05)}}});
    map.set_labels_visible(false);
    map.set_grid_visible(false);
    map.set_lighting(false);
    map.set_sun_visible(false);
    const auto plot = QRectF(map.rect()).adjusted(38, 14, -16, -44);
    map.zoom(1024, plot.center());
    ASSERT_TRUE(settle(map));
    const auto solid = pixels(map);
    const auto red_columns = [&](const QImage &image) {
        int count = 0;
        const auto ratio = map.devicePixelRatioF();
        for (int x = qCeil((plot.left() + 8) * ratio); x < qFloor((plot.right() - 8) * ratio);
             ++x) {
            const auto color = image.pixelColor(x, qRound(plot.center().y() * ratio));
            count += color.red() > 180 && color.green() < 100;
        }
        return count;
    };
    // Both original endpoints are over 150,000 logical pixels from the view;
    // every interior column must still see the clipped solid segment.
    const auto solid_columns = red_columns(solid);
    EXPECT_GT(solid_columns, (plot.width() - 18) * map.devicePixelRatioF());
    auto dashed = long_track.display;
    dashed.line_style = application::LineStyle::dashed;
    map.set_display(0, dashed, core::Epoch{0}, core::Epoch{10});
    ASSERT_TRUE(settle(map));
    const auto dashed_columns = red_columns(pixels(map));
    EXPECT_GT(dashed_columns, solid_columns / 3);
    EXPECT_LT(dashed_columns, solid_columns * 9 / 10);

    visualization::SensorMesh mesh;
    mesh.ground = {{position(-40, -20), position(40, -20), position(0, 20)}};
    mesh.color = 0x00ddff;
    mesh.opacity = .4;
    map.set_sensor_footprints(std::span(&mesh, 1));
    ASSERT_TRUE(settle(map));
    const auto filled = pixels(map);
    const auto inset = plot.adjusted(15, 15, -15, -15);
    std::optional<QColor> coverage;
    for (const auto point :
         {inset.topLeft(), inset.topRight(), inset.bottomLeft(), inset.bottomRight()}) {
        const auto pixel = (point * map.devicePixelRatioF()).toPoint();
        const auto color = filled.pixelColor(pixel);
        EXPECT_GT(color.blue(), solid.pixelColor(pixel).blue());
        EXPECT_LT(color.red(), solid.pixelColor(pixel).red());
        if (coverage)
            EXPECT_EQ(color, *coverage); // The huge facet covers the viewport once, uniformly.
        coverage = color;
    }
    capture(filled, "1024x-clipped-geometry");
}
TEST_F(GroundMapTest, HighZoomKeepsTheCursorLocationAndShowsSubDegreeAxisLabels) {
    GroundMap map(imagery_);
    configure(map);
    map.publish_tracks({});
    map.set_snapshot({core::Epoch{10},
                      core::FrameId::itrf,
                      core::OriginId::earth,
                      {{"Origin", position(0, 0)}, {"Nearby", position(.01, 0)}}});
    map.set_labels_visible(false);
    map.set_sun_visible(false);
    (void)pixels(map);
    drag(map, {37, -21});
    const auto anchor = map.marker_position(0);
    const auto separation = map.marker_position(1).x() - anchor.x();
    map.zoom(1024, anchor);
    ASSERT_EQ(map.zoom_factor(), 1024);
    EXPECT_NEAR(QLineF(map.marker_position(0), anchor).length(), 0, 1e-7);
    EXPECT_NEAR(map.marker_position(1).x() - anchor.x(), separation * 1024, 1e-7);
    ASSERT_TRUE(settle(map));
    const auto labeled = pixels(map);
    capture(labeled, "1024x-axis-labels");
    map.set_grid_visible(false);
    const auto unlabeled = pixels(map);
    const auto plot = QRectF(map.rect()).adjusted(38, 14, -16, -44);
    const auto ratio = map.devicePixelRatioF();
    int labeled_columns = 0;
    for (int x = qCeil(plot.left() * ratio); x < qFloor(plot.right() * ratio); ++x) {
        bool changed = false;
        for (int y = qCeil((plot.bottom() + 3) * ratio); y < qFloor((plot.bottom() + 19) * ratio);
             ++y)
            changed |= labeled.pixel(x, y) != unlabeled.pixel(x, y);
        labeled_columns += changed;
    }
    // A lone zero-degree label is insufficient at this scale; several readable
    // fractional-degree labels must occupy the axis instead of disappearing.
    EXPECT_GT(labeled_columns, 30 * ratio);
    ASSERT_TRUE(wheel(map, anchor, {0, -120}));
    ASSERT_TRUE(wait_zoom(map, 1024 / 1.2));
    EXPECT_NEAR(QLineF(map.marker_position(0), anchor).length(), 0, 1e-7);
}
TEST_F(GroundMapTest, ResizeAndCenteredZoomPreserveTheGeographicCenter) {
    GroundMap map(imagery_);
    configure(map);
    (void)pixels(map);
    map.zoom(3, map.marker_position(0));
    pan(map);
    // The origin marker is offset from the viewport center. Its normalized offset
    // identifies the geographic location at the center, independently of window size.
    const auto centered_offset = [&] {
        const auto plot = QRectF(map.rect()).adjusted(38, 14, -16, -44);
        const auto map_width = std::max(plot.width(), 2 * plot.height()) * map.zoom_factor();
        return (map.marker_position(0) - plot.center()) / map_width;
    };
    const auto offset = centered_offset();
    map.resize(880, 450);
    (void)pixels(map);
    EXPECT_NEAR(QLineF(centered_offset(), offset).length(), 0, 1e-9);
    EXPECT_EQ(map.zoom_factor(), 3);
    map.zoom_centered(1.3);
    ASSERT_TRUE(wait_zoom(map, 3.9));
    EXPECT_NEAR(QLineF(centered_offset(), offset).length(), 0, 1e-9);
    map.resize(400, 500);
    (void)pixels(map);
    EXPECT_NEAR(QLineF(centered_offset(), offset).length(), 0, 1e-9);
}
TEST_F(GroundMapTest, DoubleClickZoomsAtThePointerAndHomeCancelsPendingZoom) {
    GroundMap map(imagery_);
    configure(map);
    (void)pixels(map);
    const auto anchor = map.marker_position(0);
    mouse(map, QEvent::MouseButtonDblClick, anchor);
    QEvent ungrab(QEvent::UngrabMouse);
    QCoreApplication::sendEvent(&map, &ungrab);
    ASSERT_TRUE(wait_zoom(map, 2));
    EXPECT_EQ(map.marker_position(0), anchor);
    mouse(map, QEvent::MouseButtonDblClick, anchor, Qt::LeftButton, Qt::ShiftModifier);
    ASSERT_TRUE(wait_zoom(map, 1));
    key(map, Qt::Key_Plus);
    key(map, Qt::Key_Home);
    process_for(170);
    EXPECT_EQ(map.zoom_factor(), 1);
    EXPECT_EQ(map.marker_position(0), anchor);
}
TEST_F(GroundMapTest, ArrowKeysMoveTheViewportAndShiftMakesSmallerSteps) {
    GroundMap map(imagery_);
    configure(map);
    (void)pixels(map);
    map.zoom(3, map.marker_position(0));
    const auto origin = map.marker_position(0);
    key(map, Qt::Key_Right);
    const auto horizontal = origin.x() - map.marker_position(0).x();
    EXPECT_GT(horizontal, 0);
    key(map, Qt::Key_Left);
    EXPECT_NEAR(QLineF(map.marker_position(0), origin).length(), 0, 1e-9);
    key(map, Qt::Key_Right, Qt::ShiftModifier);
    EXPECT_NEAR(origin.x() - map.marker_position(0).x(), horizontal / 5, 1e-9);
    key(map, Qt::Key_Down);
    EXPECT_LT(map.marker_position(0).y(), origin.y());
    key(map, Qt::Key_Up);
    EXPECT_NEAR(map.marker_position(0).y(), origin.y(), 1e-9);
}
TEST_F(GroundMapTest, StartingADragOrHidingStopsTheZoomWithoutALateJump) {
    GroundMap map(imagery_);
    configure(map);
    map.show();
    QCoreApplication::processEvents();
    map.zoom(3, map.marker_position(0));
    const auto anchor = map.marker_position(0);
    wheel(map, anchor, {0, 120});
    const auto before_drag = map.zoom_factor();
    mouse(map, QEvent::MouseButtonPress, anchor);
    mouse(map, QEvent::MouseMove, anchor + QPointF(40, 20));
    mouse(map, QEvent::MouseButtonRelease, anchor + QPointF(40, 20));
    process_for(170);
    EXPECT_DOUBLE_EQ(map.zoom_factor(), before_drag);
    map.zoom_centered(1.3);
    const auto before_hide = map.zoom_factor();
    map.hide();
    process_for(170);
    map.show();
    QCoreApplication::processEvents();
    EXPECT_DOUBLE_EQ(map.zoom_factor(), before_hide);
}
TEST_F(GroundMapTest, AnimatedZoomSettlesToTheSameDetailedTracksAsDirectZoom) {
    GroundMap map(imagery_), fresh(imagery_);
    for (auto *view : {&map, &fresh}) {
        configure(*view);
        view->replace_track(0, dense_track());
        (void)pixels(*view);
        view->zoom(2, QPointF(250, 150));
    }
    ASSERT_TRUE(settle(map));
    wheel(map, {250, 150}, {0, 120});
    (void)pixels(map);
    ASSERT_TRUE(wait_zoom(map, 2.4));
    fresh.zoom(1.2, {250, 150});
    ASSERT_TRUE(settle(map));
    ASSERT_TRUE(settle(fresh));
    expect_same_pixels(pixels(map), pixels(fresh));
}
} // namespace orbelle::desktop
