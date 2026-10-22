#include <cmath>
#include <gtest/gtest.h>
#include <limits>
#include <numbers>
#include <orbelle/core/propagation/circular_two_body.hpp>

using namespace orbelle::core;

TEST(Epoch, KeepsAdjacentNanosecondsAtLargeEpoch) {
    EXPECT_DOUBLE_EQ(
        Epoch{8'000'000'000'000'000'001}.seconds_since(Epoch{8'000'000'000'000'000'000}), 1e-9);
    EXPECT_DOUBLE_EQ(Epoch{-1}.seconds_since(Epoch{1}), -2e-9);
}
TEST(Epoch, ExtremeDifferenceDoesNotOverflow) {
    EXPECT_GT(Epoch{std::numeric_limits<std::int64_t>::max()}.seconds_since(
                  Epoch{std::numeric_limits<std::int64_t>::min()}),
              18.0e9);
}
TEST(CircularTwoBody, InitialStateHasExplicitFrameAndUnits) {
    const CircularTwoBody orbit{Length{7e6}, GravitationalParameter{3.986004418e14}, Epoch{0}};
    const auto state = orbit.state_at(Epoch{0});
    EXPECT_EQ(state.frame, FrameId::gcrf);
    EXPECT_EQ(state.origin, OriginId::earth);
    EXPECT_EQ(state.epoch, Epoch{0});
    EXPECT_DOUBLE_EQ(state.position_m[0], 7e6);
    EXPECT_DOUBLE_EQ(state.position_m[1], 0);
    EXPECT_NEAR(state.velocity_mps[1], 7546.053290107542, 1e-9);
}
TEST(CircularTwoBody, QuarterTurnMatchesAnalyticalGeometry) {
    // Manufactured case: r=10 m, mu=1000 m^3/s^2 => angular rate exactly 1 rad/s.
    const CircularTwoBody orbit{Length{10}, GravitationalParameter{1000}, Epoch{0}};
    const auto state = orbit.state_at(Epoch{1'570'796'327});
    EXPECT_NEAR(state.position_m[0], 0, 5e-9);
    EXPECT_NEAR(state.position_m[1], 10, 5e-9);
    EXPECT_NEAR(state.velocity_mps[0], -10, 5e-9);
    EXPECT_NEAR(state.velocity_mps[1], 0, 5e-9);
}
TEST(CircularTwoBody, ConservesEnergyAndAngularMomentumInBothTimeDirections) {
    const CircularTwoBody orbit{Length{7e6}, GravitationalParameter{3.986004418e14}, Epoch{0}};
    for (const auto seconds : {-86'400LL, -300LL, 300LL, 86'400LL}) {
        const auto state = orbit.state_at(Epoch{seconds * 1'000'000'000});
        const auto &p = state.position_m;
        const auto &v = state.velocity_mps;
        const auto r = std::hypot(p[0], p[1], p[2]);
        const auto energy = (v[0] * v[0] + v[1] * v[1] + v[2] * v[2]) / 2 - 3.986004418e14 / r;
        EXPECT_NEAR(r, 7e6, 1e-8);
        EXPECT_NEAR(energy, -3.986004418e14 / 14e6, 1e-7);
        EXPECT_NEAR(p[0] * v[1] - p[1] * v[0], std::sqrt(3.986004418e14 * 7e6), 1e-4);
    }
}
TEST(CircularTwoBody, RejectsInvalidPhysicalParameters) {
    for (const auto bad : {0.0, -1.0, std::numeric_limits<double>::infinity(),
                           std::numeric_limits<double>::quiet_NaN()}) {
        EXPECT_THROW((CircularTwoBody{Length{bad}, GravitationalParameter{1}, Epoch{0}}),
                     std::invalid_argument);
        EXPECT_THROW((CircularTwoBody{Length{1}, GravitationalParameter{bad}, Epoch{0}}),
                     std::invalid_argument);
    }
}
