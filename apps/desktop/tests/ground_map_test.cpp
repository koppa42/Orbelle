#include "../src/viewer/ground_map.hpp"
#include <QApplication>
#include <QElapsedTimer>
#include <QMouseEvent>
#include <QTemporaryDir>
#include <QThread>
#include <cmath>
#include <gtest/gtest.h>

namespace orbitforge::desktop {
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
} // namespace orbitforge::desktop
