#include <cmath>
#include <gtest/gtest.h>
#include <limits>
#include <numbers>
#include <orbitforge/core/propagation/ground_station.hpp>

using namespace orbitforge::core;
TEST(GroundStation, MatchesIndependentErfaWgs84Reference) {
    // Published eraGd2gc WGS84 reference values (3.1 rad, -0.5 rad, 2500 m):
    // https://github.com/liberfa/erfa/blob/master/src/t_erfa_c.c (t_gd2gc).
    const auto actual = ground_station_position({3.1, -0.5, 2500});
    const std::array expected{-5599000.5577049947, 233011.67223479203, -3040909.4706983363};
    for (std::size_t i = 0; i < 3; ++i)
        EXPECT_NEAR(actual[i], expected[i], 1e-7);
}
TEST(GroundStation, EquatorPolesDateLineAndNegativeHeightHaveExpectedGeometry) {
    const auto equator = ground_station_position({0, 0, -400});
    EXPECT_DOUBLE_EQ(equator[0], 6377737);
    EXPECT_DOUBLE_EQ(equator[1], 0);
    EXPECT_DOUBLE_EQ(equator[2], 0);
    const auto east = ground_station_position({std::numbers::pi / 2, 0, 1000});
    EXPECT_NEAR(east[0], 0, 1e-8);
    EXPECT_NEAR(east[1], 6379137, 1e-8);
    const auto west = ground_station_position({-std::numbers::pi, 0, 0});
    const auto date_line = ground_station_position({std::numbers::pi, 0, 0});
    for (std::size_t i = 0; i < 3; ++i)
        EXPECT_NEAR(west[i], date_line[i], 1e-8);
    for (const auto sign : {-1, 1}) {
        const auto pole = ground_station_position({1, sign * std::numbers::pi / 2, 123});
        EXPECT_NEAR(pole[0], 0, 1e-8);
        EXPECT_NEAR(pole[1], 0, 1e-8);
        EXPECT_NEAR(pole[2], sign * (6356752.314245179 + 123), 1e-8);
    }
}
TEST(GroundStation, RejectsInvalidAnglesNonfiniteInputsAndUnsupportedHeights) {
    const auto nan = std::numeric_limits<double>::quiet_NaN();
    const auto inf = std::numeric_limits<double>::infinity();
    for (const auto location : {GroundStationLocation{nan, 0, 0},
                                {0, inf, 0},
                                {0, 0, nan},
                                {std::numbers::pi + 1e-12, 0, 0},
                                {0, -2, 0},
                                {0, 0, -12001},
                                {0, 0, 100001}})
        EXPECT_THROW((void)ground_station_position(location), std::invalid_argument);
}
TEST(GroundStation, IsStationaryInItrfAtExactRequestedEpochs) {
    const GroundStation station({1.2, -0.7, -350});
    const auto reference = station.state_at(Epoch{0});
    for (const auto epoch : {Epoch{-1}, Epoch{1}, Epoch{8'000'000'000'000'000'001}}) {
        const auto state = station.state_at(epoch);
        EXPECT_EQ(state.epoch, epoch);
        EXPECT_EQ(state.origin, OriginId::earth);
        EXPECT_EQ(state.frame, FrameId::itrf);
        EXPECT_EQ(state.position_m, reference.position_m);
        EXPECT_EQ(state.velocity_mps, (std::array<double, 3>{0, 0, 0}));
    }
}
