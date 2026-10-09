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
        image.fill(QColor("#284966"));
        imagery_ = directory_.filePath("earth.png");
        ASSERT_TRUE(image.save(imagery_));
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
TEST_F(GroundMapTest, RangeVisibilityReplacementAndSelectionInvalidateTrails) {
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
    EXPECT_NE(pixels(map), full);
    map.set_selection(std::nullopt);
    EXPECT_EQ(pixels(map), full);
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
    {
        const auto a = pixels(cached), b = pixels(fresh);
        int maximum = 0, changed = 0;
        for (int y = 0; y < a.height(); ++y)
            for (int x = 0; x < a.width(); ++x) {
                const auto p = a.pixelColor(x, y), q = b.pixelColor(x, y);
                const auto delta =
                    std::max({std::abs(p.red() - q.red()), std::abs(p.green() - q.green()),
                              std::abs(p.blue() - q.blue())});
                maximum = std::max(maximum, delta);
                changed += delta != 0;
            }
        // Translating a raster versus rasterizing translated floating coordinates can
        // differ by at most rounding in the antialiased edge (2/255 per channel).
        EXPECT_LE(maximum, 2) << "Changed pixels: " << changed;
    }
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
    auto long_track = track();
    long_track.points.clear();
    long_track.epochs.clear();
    for (int i = 0; i < 4000; ++i) {
        long_track.points.emplace_back(std::remainder(i * 0.7, 360.0), 50 * std::sin(i * 0.017));
        long_track.epochs.emplace_back(i * 10);
    }
    long_track.last = long_track.points.size();
    long_track.path = ground_path(long_track.points);
    map.replace_track(0, std::move(long_track));
    (void)pixels(map); // Starts an immutable worker request and draws an immediate preview.
    EXPECT_FALSE(map.tracks_ready());
    map.replace_track(0, track());
    QElapsedTimer deadline;
    deadline.start();
    while (!map.tracks_ready() && deadline.elapsed() < 2000) {
        QCoreApplication::processEvents();
        (void)pixels(map);
        QThread::msleep(1);
    }
    ASSERT_TRUE(map.tracks_ready());
    EXPECT_EQ(pixels(map), pixels(fresh));
}
} // namespace orbitforge::desktop
