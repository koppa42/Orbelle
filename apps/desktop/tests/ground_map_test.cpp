#include "../src/viewer/ground_map.hpp"
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

namespace orbitforge::desktop {
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
            if (map.tracks_ready())
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
        const auto output = qEnvironmentVariable("ORBITFORGE_MAP_CAPTURE_DIR");
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
    EXPECT_FALSE(wheel(map, {320, 20}, {0, 120})); // Letterboxing above the globe.
    EXPECT_FALSE(wheel(map, anchor, {120, 0}));
    EXPECT_FALSE(wheel(map, anchor, {}));
    for (double factor : {0.0, -1.0, std::numeric_limits<double>::infinity(),
                          std::numeric_limits<double>::quiet_NaN()})
        map.zoom(factor, anchor);
    map.zoom(2, {std::numeric_limits<double>::quiet_NaN(), 0});
    process_for(170);
    EXPECT_EQ(map.zoom_factor(), 1);
    EXPECT_EQ(map.marker_position(0), anchor);
    map.zoom(100, anchor);
    EXPECT_EQ(map.zoom_factor(), 12);
    wheel(map, anchor, {0, 120});
    EXPECT_EQ(map.zoom_factor(), 12);
    map.zoom(0.0001, anchor);
    wheel(map, anchor, {0, -120});
    EXPECT_EQ(map.zoom_factor(), 1);
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
TEST_F(GroundMapTest, MapEdgesClampWithoutADeadZoneWhenTheDragReverses) {
    GroundMap map(imagery_);
    configure(map);
    (void)pixels(map);
    map.zoom(2, map.marker_position(0));
    const auto origin = map.marker_position(0);
    mouse(map, QEvent::MouseButtonPress, origin);
    mouse(map, QEvent::MouseMove, origin + QPointF(5000, 0));
    const auto edge = map.marker_position(0);
    EXPECT_LT(edge.x(), origin.x() + 1000);
    mouse(map, QEvent::MouseMove, origin + QPointF(4990, 0));
    EXPECT_NEAR(map.marker_position(0).x(), edge.x() - 10, 1e-9);
    key(map, Qt::Key_Escape);
    EXPECT_EQ(map.marker_position(0), origin);
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
        const auto map_width = std::min(plot.width(), 2 * plot.height()) * map.zoom_factor();
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
} // namespace orbitforge::desktop
