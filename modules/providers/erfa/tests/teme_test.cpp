#include <cmath>
#include <gtest/gtest.h>
#include <numbers>
#include <orbelle/providers/erfa/provider.hpp>

using namespace orbelle;
TEST(TemeFrames, MatchesValladoRevision2AppendixC) {
    const auto time = providers::erfa::make_builtin_time_provider();
    const auto frames = providers::erfa::make_frame_provider();
    const auto epoch = time->from_utc({2004, 4, 6, 7, 51, 28, 386000000});
    constexpr double as = std::numbers::pi / (180 * 3600);
    const core::EarthOrientationState eop{
        {epoch, -32.439961, -.140682 * as, .333309 * as, 0, 0, "Vallado-appendix-C"},
        {-.0015563 / 86400, 0, 0, 0, 0}};
    const core::CartesianState teme{epoch,
                                    core::OriginId::earth,
                                    core::FrameId::teme,
                                    {5094180.16210, 6127644.65950, 6380344.53270},
                                    {-4746.131487, 785.818041, 5531.931288}};
    const auto transform = frames->teme_to_itrf_state(epoch, eop);
    const auto fixed = transform.apply(teme);
    const std::array r{-1033479.38300, 7901295.27540, 6380356.59580};
    const std::array v{-3225.636520, -2872.451450, 5531.924446};
    // A 60-digit evaluation of Appendix C eqs. C-1/C-2 at the exact printed UTC.
    const std::array precise_r{-1033479.3961736933, 7901295.273670416, 6380356.595751121};
    for (std::size_t i = 0; i < 3; ++i) {
        // Forming UTC JD + DUT1/86400 in one double shifts UT1 by 22.790 us,
        // explaining the example's 13.2 mm residual. Do not degrade split-date precision.
        EXPECT_NEAR(fixed.position_m[i], precise_r[i], .000001);
        EXPECT_NEAR(fixed.position_m[i], r[i], .02);
        EXPECT_NEAR(fixed.velocity_mps[i], v[i], .0002);
        EXPECT_NEAR(transform.inverse(fixed).velocity_mps[i], teme.velocity_mps[i], 1e-8);
    }
    const auto gcrf = core::transform_state(teme, core::FrameId::gcrf, *frames, eop);
    const auto back = core::transform_state(gcrf, core::FrameId::teme, *frames, eop);
    EXPECT_GT(std::abs(gcrf.position_m[0] - teme.position_m[0]), 100);
    for (std::size_t i = 0; i < 3; ++i) {
        EXPECT_NEAR(back.position_m[i], teme.position_m[i], 1e-8);
        EXPECT_NEAR(back.velocity_mps[i], teme.velocity_mps[i], 1e-8);
    }
}
TEST(TemeFrames, FullVelocityIncludesPolarMotionAndEarthRotationRates) {
    const auto frames = providers::erfa::make_frame_provider();
    const core::Epoch epoch{100'000'000'000};
    const core::EarthOrientationState eop{{epoch, -31.6, 1e-6, -2e-6, 0, 0, "synthetic"},
                                          {-2e-8, 1e-11, -2e-11, 0, 0}};
    const core::CartesianState state{
        epoch, core::OriginId::earth, core::FrameId::teme, {7e6, 2e6, -1e6}, {0, 7000, 1000}};
    const auto offset_position = [&](double dt) {
        auto sample = eop.sample;
        sample.epoch = core::Epoch{epoch.tai_nanoseconds() + static_cast<std::int64_t>(dt * 1e9)};
        sample.ut1_minus_tai_seconds += dt * eop.rates.ut1_minus_tai_seconds_per_second;
        sample.polar_motion_x_rad += dt * eop.rates.polar_motion_x_rad_per_second;
        sample.polar_motion_y_rad += dt * eop.rates.polar_motion_y_rad_per_second;
        auto position = state.position_m;
        for (std::size_t i = 0; i < 3; ++i)
            position[i] += dt * state.velocity_mps[i];
        return frames->teme_to_itrf(sample.epoch, sample).apply_position(position);
    };
    const auto a = offset_position(-.25), b = offset_position(.25);
    const auto result = frames->teme_to_itrf_state(epoch, eop).apply(state);
    for (std::size_t i = 0; i < 3; ++i)
        EXPECT_NEAR((b[i] - a[i]) / .5, result.velocity_mps[i], .0001);
    auto bad = eop;
    bad.sample.epoch = core::Epoch{0};
    EXPECT_THROW((void)frames->teme_to_itrf_state(epoch, bad), std::invalid_argument);
    bad = eop;
    bad.rates.polar_motion_x_rad_per_second = .1;
    EXPECT_THROW((void)frames->teme_to_itrf_state(epoch, bad), std::invalid_argument);
}
