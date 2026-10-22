#include <cmath>
#include <fstream>
#include <gtest/gtest.h>
#include <numbers>
#include <orbelle/providers/spice/provider.hpp>

using namespace orbelle;
namespace {
core::J4MeanElements initial() {
    constexpr double rad = std::numbers::pi / 180;
    return {core::Epoch{0}, 7e6, .01, 98.5 * rad, 25 * rad, 70 * rad, 10 * rad};
}
} // namespace
TEST(J4Secular, MatchesHighPrecisionReferenceOverThirtyDaysInBothDirections) {
    const auto provider = providers::spice::make_j4(initial());
    std::ifstream file(ORBELLE_J4_REFERENCE);
    ASSERT_TRUE(file);
    std::int64_t seconds = 0;
    int rows = 0;
    while (file >> seconds) {
        SCOPED_TRACE(seconds);
        const auto state = provider->state_at(core::Epoch{seconds * 1'000'000'000});
        for (double actual : state.position_m) {
            double expected = 0;
            ASSERT_TRUE(file >> expected);
            EXPECT_NEAR(actual, expected, .001);
        }
        for (double actual : state.velocity_mps) {
            double expected = 0;
            ASSERT_TRUE(file >> expected);
            EXPECT_NEAR(actual, expected, .000002);
        }
        ++rows;
    }
    EXPECT_EQ(rows, 5);
}
TEST(J4Secular, ZeroHarmonicsReduceToTwoBodyAndJ2SquaredIsRetained) {
    auto body = core::j4_earth();
    body.j2 = body.j4 = 0;
    const auto ellipse = providers::spice::j2_mean_ellipse_state(initial(), body);
    const auto two = providers::spice::make_two_body(ellipse, body.mu);
    const auto four = providers::spice::make_j4(initial(), body);
    const auto epoch = core::Epoch{86'400'000'000'000};
    const auto a = two->state_at(epoch), b = four->state_at(epoch);
    for (std::size_t i = 0; i < 3; ++i) {
        EXPECT_NEAR(a.position_m[i], b.position_m[i], .001);
        EXPECT_NEAR(a.velocity_mps[i], b.velocity_mps[i], .000002);
    }
    body.j2 = core::j4_earth().j2;
    const auto j2 = providers::spice::make_j2(initial(), body, {})->state_at(epoch);
    const auto j22 = providers::spice::make_j4(initial(), body)->state_at(epoch);
    EXPECT_GT(std::hypot(j2.position_m[0] - j22.position_m[0], j2.position_m[1] - j22.position_m[1],
                         j2.position_m[2] - j22.position_m[2]),
              1);
}
TEST(J4Secular, VelocityDifferentiatesTheTrajectoryAtPolarEquatorialAndEccentricLimits) {
    for (double i : {0.0, std::numbers::pi / 2, std::numbers::pi}) {
        for (double e : {0.0, .7}) {
            auto input = initial();
            input.inclination_rad = i;
            input.eccentricity = e;
            input.semi_major_axis_m = e > .5 ? 30e6 : 7e6;
            const auto provider = providers::spice::make_j4(input);
            constexpr std::int64_t ns = 86'400'000'000'000;
            const auto a = provider->state_at(core::Epoch{ns - 125'000'000});
            const auto b = provider->state_at(core::Epoch{ns + 125'000'000});
            const auto value = provider->state_at(core::Epoch{ns});
            for (std::size_t k = 0; k < 3; ++k)
                EXPECT_NEAR((b.position_m[k] - a.position_m[k]) / .25, value.velocity_mps[k],
                            .0001);
        }
    }
    auto bad = initial();
    bad.eccentricity = 1;
    EXPECT_THROW((void)providers::spice::make_j4(bad), std::invalid_argument);
    bad = initial();
    bad.semi_major_axis_m = 6e6;
    EXPECT_THROW((void)providers::spice::make_j4(bad), std::invalid_argument);
}
