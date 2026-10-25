#include "../src/viewer/animation_timing.hpp"
#include <gtest/gtest.h>
#include <limits>

namespace orbelle::desktop {
TEST(AnimationTiming, PlaybackRetainsSubMillisecondTimeAcrossFramesAndPauses) {
    PlaybackClock clock;
    constexpr std::int64_t interval = 6'250'001;
    double seconds = 0;
    for (int i = 1; i <= 1600; ++i)
        seconds += clock.advance(i * interval);
    EXPECT_NEAR(seconds, 1600 * interval * 1e-9, 1e-11);
    clock.reset(30'000'000'000);
    EXPECT_NEAR(clock.advance(30'008'333'333), .008333333, 1e-12);
    clock.reset(31'000'000'000);
    EXPECT_DOUBLE_EQ(clock.advance(31'000'000'000), 0);
    EXPECT_DOUBLE_EQ(clock.advance(31'250'000'000), .25);
}
TEST(AnimationTiming, PacingTracksFractionalRefreshPeriodsWithoutAccumulatingRounding) {
    for (const auto rate : {60.0, 120.0, 144.0, 160.0, 180.0, 240.0}) {
        SCOPED_TRACE(rate);
        FramePacer pacer;
        std::int64_t now = 0;
        constexpr int frames = 1000;
        for (int i = 0; i < frames; ++i) {
            const auto delay = pacer.delay_ms(now, rate);
            ASSERT_GE(delay, 1);
            now += delay * 1'000'000LL;
            if (i + 1 != frames)
                now += 500'000; // Work and event processing consume part of the next slot.
        }
        EXPECT_NEAR(static_cast<double>(now),
                    static_cast<double>(frames * std::llround(1e9 / rate)), 1'000'000);
    }
}
TEST(AnimationTiming, SlowFramesSkipMissedSlotsAndScreenChangesResetCadence) {
    FramePacer pacer;
    EXPECT_EQ(pacer.delay_ms(0, 160), 7);
    const auto after_stall = pacer.delay_ms(201'000'000, 160);
    EXPECT_GE(after_stall, 1);
    EXPECT_LE(after_stall, 7);
    EXPECT_EQ(pacer.delay_ms(202'000'000, 60), 17);
    pacer.reset();
    EXPECT_EQ(pacer.delay_ms(500'000'000, 180), 6);
    for (const auto rate : {0.0, -1.0, std::numeric_limits<double>::quiet_NaN()}) {
        pacer.reset();
        EXPECT_EQ(pacer.delay_ms(0, rate), 17);
    }
    pacer.reset();
    EXPECT_EQ(pacer.delay_ms(0, 1000), 5);
}
TEST(AnimationTiming, EarlyTimerDoesNotScheduleAnExtraFrameAgainstTheSameDeadline) {
    FramePacer pacer;
    EXPECT_EQ(pacer.delay_ms(0, 160), 7);
    EXPECT_EQ(pacer.delay_ms(6'100'000, 160), 7);
    EXPECT_EQ(pacer.delay_ms(12'300'000, 160), 7);
}
} // namespace orbelle::desktop
