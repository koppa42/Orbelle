#include <cmath>
#include <gtest/gtest.h>
#include <limits>
#include <numbers>
#include <orbelle/core/propagation/sensor.hpp>
using namespace orbelle::core;
namespace {
constexpr double d = std::numbers::pi / 180;
SensorVector ray(double cone, double clock = 0) {
    return {std::sin(cone * d) * std::cos(clock * d), std::sin(cone * d) * std::sin(clock * d),
            std::cos(cone * d)};
}
} // namespace
TEST(Sensor, ConesBlindRegionAndWrappedClock) {
    SensorPattern p(45 * d);
    SensorGeometry cone(p);
    EXPECT_NEAR(cone.margin(ray(20)), 25 * d, 1e-14);
    EXPECT_LT(cone.margin(ray(46)), 0);
    p.type = SensorPatternType::complex_conic;
    p.inner_half_angle_rad = 10 * d;
    p.minimum_clock_rad = 300 * d;
    p.maximum_clock_rad = 60 * d;
    SensorGeometry sector(p);
    EXPECT_LT(sector.margin(ray(5)), 0);
    EXPECT_GT(sector.margin(ray(20)), 0);
    EXPECT_LT(sector.margin(ray(20, 180)), 0);
    EXPECT_NEAR(sector.margin(ray(20, 60)), 0, 1e-14);
    p.minimum_clock_rad = 0;
    p.maximum_clock_rad = 360 * d;
    EXPECT_GT(SensorGeometry(p).margin(ray(20, 180)), 0);
    p.minimum_clock_rad = p.maximum_clock_rad = 20 * d;
    EXPECT_THROW((void)SensorGeometry(p), std::invalid_argument);
}
TEST(Sensor, RectangleFourPlanesAndRoll) {
    SensorPattern p;
    p.type = SensorPatternType::rectangular;
    p.horizontal_half_angle_rad = 30 * d;
    p.vertical_half_angle_rad = 10 * d;
    SensorGeometry rectangle(p);
    EXPECT_GT(rectangle.margin(ray(20, 0)), 0);
    EXPECT_LT(rectangle.margin(ray(20, 90)), 0);
    EXPECT_NEAR(rectangle.margin({std::tan(30 * d), std::tan(10 * d), 1}), 0, 1e-14);
    EXPECT_LT(rectangle.margin({0, 0, -1}), 0);
    const auto axes = sensor_axes({1, 0, 0}, {0, 0, 1}, 90 * d);
    EXPECT_GT(rectangle.margin(axes.to_local(ray(20, 90))), 0);
    EXPECT_LT(rectangle.margin(axes.to_local(ray(20, 0))), 0);
    const auto fallback = sensor_axes({0, 0, 0}, {0, 0, 1});
    EXPECT_EQ(fallback.z, (SensorVector{0, 0, 1}));
    EXPECT_THROW((void)sensor_axes({1, 0, 0}, {0, 0, 0}), std::invalid_argument);
}
TEST(Sensor, IntrinsicXyzRotationMatchesAnalyticBasisAndFinalBoresight) {
    // Independently expanded Rx(30) Ry(45) Rz(60), expressed with exact radicals.
    const double s2 = std::sqrt(2.), s3 = std::sqrt(3.), s6 = std::sqrt(6.);
    const SensorAxes expected{{s2 / 4, .75 + s2 / 8, s3 / 4 - s6 / 8},
                              {-s6 / 4, s3 / 4 - s6 / 8, .25 + 3 * s2 / 8},
                              {s2 / 2, -s2 / 4, s6 / 4}};
    const auto actual = sensor_axes({1, 0, 0}, {0, 0, 1}, 30 * d, 45 * d, 60 * d);
    const auto no_roll = sensor_axes({1, 0, 0}, {0, 0, 1}, 30 * d, 45 * d, 0);
    for (std::size_t i = 0; i < 3; ++i) {
        EXPECT_NEAR(actual.x[i], expected.x[i], 2e-15);
        EXPECT_NEAR(actual.y[i], expected.y[i], 2e-15);
        EXPECT_NEAR(actual.z[i], expected.z[i], 2e-15);
        EXPECT_DOUBLE_EQ(actual.z[i], no_roll.z[i]);
    }
    const auto x_only = sensor_axes({1, 0, 0}, {0, 0, 1}, 90 * d, 0, 0);
    EXPECT_NEAR(x_only.z[1], -1, 1e-15);
    const auto y_only = sensor_axes({1, 0, 0}, {0, 0, 1}, 0, 90 * d, 0);
    EXPECT_NEAR(y_only.z[0], 1, 1e-15);
    const auto roll_only = sensor_axes({1, 0, 0}, {0, 0, 1}, 0, 0, 90 * d);
    EXPECT_NEAR(roll_only.x[1], 1, 1e-15);
    EXPECT_EQ(roll_only.z, (SensorVector{0, 0, 1}));
}
TEST(Sensor, TiltedAxesRemainRightHandedAtSingularAnglesAndRejectNonfiniteOffsets) {
    for (const SensorVector velocity :
         {SensorVector{}, SensorVector{2, -3, 5}, SensorVector{1, 2, 3}}) {
        for (const double tilt : {-90 * d, 90 * d, 360 * d, 12.34}) {
            const auto axes = sensor_axes(velocity, {2, -3, 5}, .43, tilt, -.72);
            const auto local_x = axes.to_local(axes.x), local_y = axes.to_local(axes.y),
                       local_z = axes.to_local(axes.z);
            const SensorVector cross{axes.x[1] * axes.y[2] - axes.x[2] * axes.y[1],
                                     axes.x[2] * axes.y[0] - axes.x[0] * axes.y[2],
                                     axes.x[0] * axes.y[1] - axes.x[1] * axes.y[0]};
            const SensorVector v{2, -3, 5};
            const auto round_trip = axes.to_world(axes.to_local(v));
            for (std::size_t i = 0; i < 3; ++i) {
                EXPECT_NEAR(local_x[i], i == 0 ? 1 : 0, 2e-15);
                EXPECT_NEAR(local_y[i], i == 1 ? 1 : 0, 2e-15);
                EXPECT_NEAR(local_z[i], i == 2 ? 1 : 0, 2e-15);
                EXPECT_NEAR(cross[i], axes.z[i], 2e-15);
                EXPECT_NEAR(round_trip[i], v[i], 1e-14);
            }
        }
    }
    for (const double bad :
         {std::numeric_limits<double>::quiet_NaN(), std::numeric_limits<double>::infinity()}) {
        EXPECT_THROW((void)sensor_axes({1, 0, 0}, {0, 0, 1}, bad, 0, 0), std::invalid_argument);
        EXPECT_THROW((void)sensor_axes({1, 0, 0}, {0, 0, 1}, 0, bad, 0), std::invalid_argument);
        EXPECT_THROW((void)sensor_axes({1, 0, 0}, {0, 0, 1}, 0, 0, bad), std::invalid_argument);
    }
}
TEST(Sensor, CustomConcavityAndOffAxisBoundary) {
    SensorPattern p;
    p.type = SensorPatternType::custom;
    // L-shaped polygon in the gnomonic plane, excluding the upper-right corner.
    for (const auto xy : std::vector<std::array<double, 2>>{
             {-.5, -.5}, {.5, -.5}, {.5, 0}, {0, 0}, {0, .5}, {-.5, .5}}) {
        auto clock = std::atan2(xy[1], xy[0]);
        if (clock < 0)
            clock += 2 * std::numbers::pi;
        p.custom_points.push_back({std::atan(std::hypot(xy[0], xy[1])), clock});
    }
    SensorGeometry polygon(p);
    EXPECT_GT(polygon.margin({-.25, .25, 1}), 0);
    EXPECT_LT(polygon.margin({.25, .25, 1}), 0);
    EXPECT_NEAR(polygon.margin({0, .25, 1}), 0, 1e-14);
    EXPECT_EQ(polygon.triangles(3 * d).size(), 4U);
    p.custom_points = {{20 * d, 0}, {20 * d, 180 * d}, {20 * d, 90 * d}, {20 * d, 270 * d}};
    EXPECT_THROW((void)SensorGeometry(p), std::invalid_argument);
    p.custom_points = {{20 * d, 0}, {30 * d, 0}, {30 * d, 20 * d}, {20 * d, 20 * d}};
    EXPECT_LT(SensorGeometry(p).margin({0, 0, 1}), 0);
    EXPECT_GT(SensorGeometry(p).margin(ray(25, 10)), 0);
}
TEST(Sensor, PatternImportIsExplicitClosedAndBounded) {
    const std::string text =
        "stk.v.12.5\nNumberPoints 5\nInterpolateAzElPts No\nHalfAngleAzimuthData\n30 0\n30 90\n30 "
        "180\n30 270\n30 0\nEndPatternData\n";
    auto p = parse_sensor_pattern(text);
    EXPECT_EQ(p.custom_points.size(), 4U);
    EXPECT_GT(SensorGeometry(p).margin({0, 0, 1}), 0);
    auto bad = text;
    bad.erase(bad.find("InterpolateAzElPts"), 22);
    EXPECT_THROW((void)parse_sensor_pattern(bad), std::invalid_argument);
    EXPECT_THROW((void)parse_sensor_pattern(text + "unsupported\n"), std::invalid_argument);
    const auto reference =
        parse_sensor_pattern("stk.v.12.5\nNumberPoints 5\nReferenceDistance 10\nPatternData\n5 "
                             "0\n5 90\n5 180\n5 270\n5 0\nEndPatternData\n");
    EXPECT_NEAR(reference.custom_points[0].cone_rad, std::atan(.5), 1e-15);
}
