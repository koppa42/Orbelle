#include <cmath>
#include <gtest/gtest.h>
#include <limits>
#include <numbers>
#include <orbelle/core/propagation/access.hpp>
#include <orbelle/core/propagation/ground_station.hpp>

using namespace orbelle::core;
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
TEST(GroundStation, CoordinateRepresentationsMatchReferenceAndRetainAboveGroundHeight) {
    const GroundStationLocation location{3.1, -0.5, 2400, 100};
    const auto xyz = ground_station_coordinates({3.1, -0.5, 2500}, GroundPositionType::cartesian);
    const auto inverse =
        ground_station_location(GroundPositionType::cartesian,
                                {-5599000.5577049947, 233011.67223479203, -3040909.4706983363});
    EXPECT_NEAR(inverse.longitude_rad, 3.1, 1e-14);
    EXPECT_NEAR(inverse.latitude_rad, -0.5, 1e-14);
    EXPECT_NEAR(inverse.height_m, 2500, 1e-7);
    const auto actual = ground_station_position(location);
    for (int i = 0; i < 3; ++i)
        EXPECT_NEAR(actual[i], xyz[i], 1e-7);
    for (auto type : {GroundPositionType::geodetic, GroundPositionType::geocentric,
                      GroundPositionType::cartesian, GroundPositionType::spherical,
                      GroundPositionType::cylindrical}) {
        const auto converted = ground_station_location(
            type, ground_station_coordinates(location, type), location.height_above_ground_m);
        EXPECT_NEAR(converted.longitude_rad, location.longitude_rad, 1e-14);
        EXPECT_NEAR(converted.latitude_rad, location.latitude_rad, 1e-14);
        EXPECT_NEAR(converted.height_m, location.height_m, 1e-7);
        EXPECT_EQ(converted.height_above_ground_m, 100);
    }
}
TEST(GroundStation, GeocentricSubpointLatitudeDiffersFromSphericalVectorLatitude) {
    const GroundStationLocation location{0.4, std::numbers::pi / 4, 80000};
    constexpr double flattening = 1.0 / 298.257223563;
    const double subpoint_latitude = std::atan((1 - flattening) * (1 - flattening));
    const auto geocentric = ground_station_coordinates(location, GroundPositionType::geocentric);
    const auto spherical = ground_station_coordinates(location, GroundPositionType::spherical);
    EXPECT_NEAR(geocentric[0], subpoint_latitude, 1e-15);
    EXPECT_EQ(geocentric[2], 80000);
    EXPECT_GT(spherical[0], geocentric[0] + 1e-5);
    EXPECT_GT(spherical[2], 6e6);
}
TEST(GroundStation, CoordinateInverseSupportsPolesDatelineAndHeightBoundaries) {
    for (const auto latitude : {-std::numbers::pi / 2, -1.2, 0.0, 0.7, std::numbers::pi / 2})
        for (const auto height : {-12000.0, -400.0, 0.0, 100000.0})
            for (auto type : {GroundPositionType::cartesian, GroundPositionType::spherical,
                              GroundPositionType::cylindrical}) {
                const GroundStationLocation original{std::numbers::pi, latitude, height};
                const auto converted =
                    ground_station_location(type, ground_station_coordinates(original, type));
                EXPECT_NEAR(converted.latitude_rad, latitude, 1e-14);
                EXPECT_NEAR(converted.height_m, height, 1e-7);
                const auto p = ground_station_position(original),
                           q = ground_station_position(converted);
                for (int i = 0; i < 3; ++i)
                    EXPECT_NEAR(p[i], q[i], 1e-7);
            }
}
TEST(GroundStation, RejectsInvalidCoordinateTypesVectorsAndAboveGroundHeights) {
    const auto nan = std::numeric_limits<double>::quiet_NaN();
    for (const auto above : {-1.0, 100001.0, nan})
        EXPECT_THROW((void)ground_station_position({0, 0, 0, above}), std::invalid_argument);
    EXPECT_THROW((void)ground_station_position({0, 0, 99999, 2}), std::invalid_argument);
    EXPECT_THROW((void)ground_station_location(GroundPositionType::cartesian, {0, 0, 0}),
                 std::invalid_argument);
    EXPECT_THROW((void)ground_station_location(GroundPositionType::cartesian, {1e308, 0, 0}),
                 std::invalid_argument);
    EXPECT_THROW((void)ground_station_location(GroundPositionType::spherical, {0, 0, -6378137}),
                 std::invalid_argument);
    EXPECT_THROW((void)ground_station_location(GroundPositionType::cylindrical, {-1, 0, 6378137}),
                 std::invalid_argument);
    EXPECT_THROW((void)ground_station_location(GroundPositionType::geocentric, {2, 0, 0}),
                 std::invalid_argument);
    EXPECT_THROW((void)ground_station_coordinates({}, static_cast<GroundPositionType>(99)),
                 std::invalid_argument);
    EXPECT_THROW((void)ground_station_location(static_cast<GroundPositionType>(99), {0, 0, 0}),
                 std::invalid_argument);
}
TEST(GroundStation, LocalGroundAndTowerHeightHaveDifferentHorizonAndRange) {
    // Equatorial slice is a circle of radius a + ground height. A 10 m tower can
    // see a nearby lower target but cannot look through its own local ground.
    for (double ground : {-400.0, 2000.0}) {
        const double r = 6378137 + ground;
        CartesianState target{Epoch{0}, OriginId::earth, FrameId::itrf, {r + 5, 100, 0}, {0, 0, 0}};
        auto observation = observe_ground_target({0, 0, ground, 10}, target);
        EXPECT_TRUE(observation.line_of_sight);
        EXPECT_NEAR(observation.range_m, std::hypot(5, 100), 1e-8);
        EXPECT_NEAR(observation.elevation_rad, std::atan2(-5, 100), 1e-14);
        target.position_m = {r - 5, 100, 0};
        EXPECT_FALSE(observe_ground_target({0, 0, ground, 10}, target).line_of_sight);
        target.position_m = {r + 1000, 0, 0};
        EXPECT_TRUE(observe_ground_target({0, 0, ground}, target).line_of_sight);
        // Elevation below the local horizon is blocked without a tower, even
        // at a mountain site where a ray would clear the central-body ellipsoid.
        target.position_m = {r - 1, 100, 0};
        EXPECT_FALSE(observe_ground_target({0, 0, ground}, target).line_of_sight);
    }
}
