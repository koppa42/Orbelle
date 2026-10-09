#include "../src/viewer/mission_timeline.hpp"
#include <QApplication>
#include <QKeyEvent>
#include <QMouseEvent>
#include <QWheelEvent>
#include <algorithm>
#include <cmath>
#include <gtest/gtest.h>
#include <orbitforge/runtime/time_frames.hpp>

namespace orbitforge::desktop {
namespace {
constexpr qint64 second = 1000, hour = 3'600'000, day = 86'400'000;
qint64 utc(core::UtcDateTime value) {
    static const auto provider = runtime::make_builtin_time_provider();
    return provider->from_utc(value).tai_nanoseconds() / 1'000'000;
}
class Timeline : public ::testing::Test {
  protected:
    void SetUp() override {
        // List-test discovery needs no Qt plugin. Only actual tests initialize Qt.
        static int argc = 1;
        static char executable[] = "timeline_test";
        static char *argv[] = {executable, nullptr};
        static QApplication application(argc, argv);
    }
    static void key(MissionTimeline &widget, int value) {
        QKeyEvent event(QEvent::KeyPress, value, Qt::NoModifier);
        QCoreApplication::sendEvent(&widget, &event);
    }
    static void drag(MissionTimeline &widget, QPointF from, QPointF to,
                     Qt::KeyboardModifiers modifiers = Qt::NoModifier) {
        QMouseEvent press(QEvent::MouseButtonPress, from, widget.mapToGlobal(from), Qt::LeftButton,
                          Qt::LeftButton, modifiers);
        QMouseEvent move(QEvent::MouseMove, to, widget.mapToGlobal(to), Qt::NoButton,
                         Qt::LeftButton, modifiers);
        QMouseEvent release(QEvent::MouseButtonRelease, to, widget.mapToGlobal(to), Qt::LeftButton,
                            Qt::NoButton, modifiers);
        QCoreApplication::sendEvent(&widget, &press);
        QCoreApplication::sendEvent(&widget, &move);
        QCoreApplication::sendEvent(&widget, &release);
    }
};
TEST_F(Timeline, ZoomedPointerAndKeyboardSeekKeepMillisecondPrecision) {
    MissionTimeline widget;
    widget.resize(1000, 144);
    const auto first = utc({2026, 10, 9, 0, 0, 0, 0});
    widget.set_interval(first, first + day, "Scenario", "Satellite");
    widget.set_current_time(first + 12 * hour + 123);
    widget.zoom(1'000'000);
    ASSERT_EQ(widget.visible_last() - widget.visible_first(), second);
    std::optional<qint64> requested;
    widget.seek_requested = [&](qint64 time) { requested = time; };
    const auto plot = widget.detail_rect();
    const auto point = QPointF(plot.left() + plot.width() * 0.371, plot.center().y());
    const auto expected = widget.visible_first() + 371;
    drag(widget, point, point);
    ASSERT_TRUE(requested);
    EXPECT_EQ(*requested, expected);
    EXPECT_EQ(widget.current_time(), expected);
    key(widget, Qt::Key_Right);
    EXPECT_EQ(*requested, expected + 1);
    key(widget, Qt::Key_Left);
    EXPECT_EQ(*requested, expected);
    key(widget, Qt::Key_Home);
    EXPECT_EQ(*requested, first);
    EXPECT_EQ(widget.visible_first(), first);
    key(widget, Qt::Key_End);
    EXPECT_EQ(*requested, first + day);
    EXPECT_EQ(widget.visible_last(), first + day);
}
TEST_F(Timeline, OverviewResizesAndPansWithoutChangingSimulationTime) {
    MissionTimeline widget;
    widget.resize(1000, 144);
    const auto first = utc({2026, 10, 9, 0, 0, 0, 0});
    widget.set_interval(first, first + day, "Scenario");
    widget.set_current_time(first + 12 * hour);
    widget.zoom(4);
    int seeks = 0;
    widget.seek_requested = [&](qint64) { ++seeks; };
    const auto overview = widget.overview_rect();
    const auto at = [&](double fraction) {
        return QPointF(overview.left() + overview.width() * fraction, overview.center().y());
    };
    drag(widget, at(0.375), at(0.25));
    EXPECT_EQ(widget.visible_first(), first + 6 * hour);
    EXPECT_EQ(widget.visible_last(), first + 15 * hour);
    drag(widget, at(0.625), at(0.75));
    EXPECT_EQ(widget.visible_first(), first + 6 * hour);
    EXPECT_EQ(widget.visible_last(), first + 18 * hour);
    drag(widget, at(0.5), at(0.625));
    EXPECT_EQ(widget.visible_first(), first + 9 * hour);
    EXPECT_EQ(widget.visible_last(), first + 21 * hour);
    EXPECT_EQ(widget.current_time(), first + 12 * hour);
    EXPECT_EQ(seeks, 0);
    // Dragging beyond the scenario keeps both the duration and endpoints bounded.
    drag(widget, at(0.625), at(1.2));
    EXPECT_EQ(widget.visible_first(), first + 12 * hour);
    EXPECT_EQ(widget.visible_last(), first + day);
}
TEST_F(Timeline, WheelAnchorsAtPointerAndShiftDragOnlyBrowses) {
    MissionTimeline widget;
    widget.resize(1000, 144);
    const auto first = utc({2026, 10, 9, 0, 0, 0, 0});
    widget.set_interval(first, first + day, "Scenario");
    widget.set_current_time(first + 12 * hour);
    int seeks = 0;
    widget.seek_requested = [&](qint64) { ++seeks; };
    const auto plot = widget.detail_rect();
    const auto point = QPointF(plot.left() + plot.width() / 4, plot.center().y());
    QWheelEvent event(point, widget.mapToGlobal(point), {}, {0, 120}, Qt::NoButton, Qt::NoModifier,
                      Qt::NoScrollPhase, false);
    QCoreApplication::sendEvent(&widget, &event);
    const auto span = widget.visible_last() - widget.visible_first();
    EXPECT_LT(span, day);
    EXPECT_NEAR(widget.visible_first() + span * 0.25, first + 6 * hour, 1);
    drag(widget, point, point + QPointF(plot.width() * 0.05, 0), Qt::ShiftModifier);
    EXPECT_EQ(widget.visible_last() - widget.visible_first(), span);
    EXPECT_EQ(widget.current_time(), first + 12 * hour);
    EXPECT_EQ(seeks, 0);
    key(widget, Qt::Key_F);
    EXPECT_EQ(widget.visible_first(), first);
    EXPECT_EQ(widget.visible_last(), first + day);
}
TEST_F(Timeline, SelectionAndScaleChangesPreserveViewAndPhysicalTime) {
    MissionTimeline widget;
    const auto first = utc({2026, 10, 9, 0, 0, 0, 0});
    widget.set_interval(first, first + day, "Scenario", "A");
    widget.set_current_time(first + 12 * hour + 321);
    widget.zoom(4);
    const auto left = widget.visible_first(), right = widget.visible_last();
    for (const auto scale : {CalendarScale::tai, CalendarScale::tt, CalendarScale::utc}) {
        widget.set_interval(first, first + day, "Scenario", "B");
        widget.set_scale(scale);
        EXPECT_EQ(widget.visible_first(), left);
        EXPECT_EQ(widget.visible_last(), right);
        EXPECT_EQ(widget.current_time(), first + 12 * hour + 321);
        for (auto tick : widget.ruler_ticks())
            EXPECT_TRUE(calendar_text(core::Epoch{tick * 1'000'000}, scale).endsWith(".000"));
    }
    widget.set_interval(first + day, first + 2 * day, "New scenario");
    EXPECT_EQ(widget.visible_first(), first + day);
    EXPECT_EQ(widget.visible_last(), first + 2 * day);
}
TEST_F(Timeline, UtcDailyTicksAlignToMidnightOnBothSidesOfLeapSecond) {
    MissionTimeline widget;
    widget.resize(600, 144);
    const auto first = utc({2016, 12, 30, 0, 0, 0, 0});
    const auto last = utc({2017, 1, 2, 0, 0, 0, 0});
    widget.set_interval(first, last, "Leap second");
    const auto ticks = widget.ruler_ticks();
    ASSERT_EQ(ticks.size(), 4);
    EXPECT_EQ(ticks[0], first);
    EXPECT_EQ(ticks[1] - ticks[0], day);
    EXPECT_EQ(ticks[2] - ticks[1], day + second);
    EXPECT_EQ(ticks[3], last);
    for (auto tick : ticks)
        EXPECT_EQ(calendar_text(core::Epoch{tick * 1'000'000}, CalendarScale::utc, false),
                  "00:00:00");
}
TEST_F(Timeline, FineUtcRulerIncludesLeapSecondAndFractionalLeapInterval) {
    MissionTimeline widget;
    widget.resize(1000, 144);
    const auto first = utc({2016, 12, 31, 23, 59, 58, 0});
    const auto leap = utc({2016, 12, 31, 23, 59, 60, 0});
    const auto last = utc({2017, 1, 1, 0, 0, 2, 0});
    widget.set_interval(first, last, "Leap second");
    auto ticks = widget.ruler_ticks();
    ASSERT_EQ(ticks.size(), 6);
    EXPECT_EQ(ticks[2], leap);
    for (std::size_t i = 1; i < ticks.size(); ++i)
        EXPECT_EQ(ticks[i] - ticks[i - 1], second);
    widget.set_interval(leap + 100, leap + 900, "Fractional leap second");
    ticks = widget.ruler_ticks();
    ASSERT_EQ(ticks.size(), 9);
    EXPECT_EQ(ticks.front(), leap + 100);
    EXPECT_EQ(ticks.back(), leap + 900);
    for (auto tick : ticks)
        EXPECT_TRUE(
            calendar_text(core::Epoch{tick * 1'000'000}, CalendarScale::utc).contains("23:59:60."));
}
TEST_F(Timeline, VeryShortScenarioAndZoomStayWithinEndpoints) {
    MissionTimeline widget;
    const auto first = utc({2026, 10, 9, 0, 0, 0, 0});
    widget.set_interval(first, first + 2, "Two milliseconds");
    widget.zoom(1e12);
    EXPECT_EQ(widget.visible_first(), first);
    EXPECT_EQ(widget.visible_last(), first + 2);
    key(widget, Qt::Key_Right);
    EXPECT_EQ(widget.current_time(), first + 1);
    key(widget, Qt::Key_Right);
    key(widget, Qt::Key_Right);
    EXPECT_EQ(widget.current_time(), first + 2);
}
} // namespace
} // namespace orbitforge::desktop
