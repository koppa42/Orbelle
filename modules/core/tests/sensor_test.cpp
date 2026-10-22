#include <cmath>
#include <gtest/gtest.h>
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
