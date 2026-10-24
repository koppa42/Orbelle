#include <gtest/gtest.h>
#ifdef ORBELLE_HAS_VISUALIZATION
#include <cmath>
#include <numbers>
#include <orbelle/visualization/sensor_view.hpp>
using namespace orbelle;
namespace {
constexpr double d = std::numbers::pi / 180;
void on_earth(const visualization::SensorMesh &mesh) {
    for (const auto &triangle : mesh.ground)
        for (const auto &p : triangle) {
            const auto value = (p[0] * p[0] + p[1] * p[1]) / (6378137. * 6378137.) +
                               p[2] * p[2] / (6356752.314245179 * 6356752.314245179);
            ASSERT_NEAR(value, 1, 2e-12);
            for (double x : p)
                ASSERT_TRUE(std::isfinite(x));
        }
}
} // namespace
TEST(SensorView, FootprintIncludesEarthWhenEveryBoundaryRayMisses) {
    const auto axes = core::sensor_axes({0, 7500, 0}, {-1, 0, 0});
    auto mesh =
        visualization::SensorView(core::SensorPattern(80 * d)).project({7e6, 0, 0}, axes, 2e7);
    ASSERT_FALSE(mesh.ground.empty());
    ASSERT_FALSE(mesh.ground_edges.empty());
    on_earth(mesh);
    double min_y = 0, max_y = 0;
    for (const auto &t : mesh.ground)
        for (const auto &p : t) {
            min_y = std::min(min_y, p[1]);
            max_y = std::max(max_y, p[1]);
        }
    EXPECT_LT(min_y, -2e6);
    EXPECT_GT(max_y, 2e6);
}
TEST(SensorView, AnnularBlindRegionAndSpaceDirectionRemainEmpty) {
    core::SensorPattern p(30 * d);
    p.type = core::SensorPatternType::complex_conic;
    p.inner_half_angle_rad = 10 * d;
    const core::SensorVector position{7e6, 0, 0};
    const auto axes = core::sensor_axes({0, 7500, 0}, {-1, 0, 0});
    const auto mesh = visualization::SensorView(p).project(position, axes, 2e7);
    ASSERT_FALSE(mesh.ground.empty());
    on_earth(mesh);
    for (const auto &t : mesh.ground)
        for (const auto &v : t) {
            const core::SensorVector ray{v[0] - position[0], v[1], v[2]};
            EXPECT_GT(core::SensorGeometry(p).margin(axes.to_local(ray)), -.001);
            EXPECT_GT(std::hypot(v[1], v[2]), 100000);
        }
    const auto away = core::sensor_axes({0, 7500, 0}, {1, 0, 0});
    EXPECT_TRUE(visualization::SensorView(p).project(position, away, 2e7).ground.empty());
}
TEST(SensorView, PolarRectangularFootprintAndPartialLimb) {
    core::SensorPattern p;
    p.type = core::SensorPatternType::rectangular;
    p.horizontal_half_angle_rad = 80 * d;
    p.vertical_half_angle_rad = 20 * d;
    const auto mesh = visualization::SensorView(p).project(
        {0, 0, 7e6}, core::sensor_axes({7500, 0, 0}, {.8, 0, -1}), 2e7);
    ASSERT_FALSE(mesh.ground.empty());
    ASSERT_FALSE(mesh.sides.empty());
    on_earth(mesh);
}
TEST(SensorView, LocalXAndYTiltsMoveTheGroundFootprintInDifferentDirections) {
    const visualization::SensorView view(core::SensorPattern(2 * d));
    const auto check_tilt = [&](double x, double y) {
        const auto axes = core::sensor_axes({0, 7500, 0}, {-1, 0, 0}, x, y, 0);
        const auto mesh = view.project({7e6, 0, 0}, axes, 2e7);
        EXPECT_FALSE(mesh.ground.empty());
        on_earth(mesh);
        for (const auto &triangle : mesh.ground) {
            for (const auto &point : triangle) {
                if (x > 0) {
                    EXPECT_GT(point[2], 100000);
                } else {
                    EXPECT_GT(point[1], 100000);
                }
                const core::SensorVector ray{point[0] - 7e6, point[1], point[2]};
                EXPECT_GE(
                    core::SensorGeometry(core::SensorPattern(2 * d)).margin(axes.to_local(ray)),
                    -.001);
            }
        }
    };
    check_tilt(20 * d, 0);
    check_tilt(0, 20 * d);
    const auto away = core::sensor_axes({0, 7500, 0}, {-1, 0, 0}, 180 * d, 0, 0);
    EXPECT_TRUE(view.project({7e6, 0, 0}, away, 2e7).ground.empty());
}
TEST(SensorView, SpaceExtensionDoesNotLimitEarthIntersections) {
    const auto mesh =
        visualization::SensorView(core::SensorPattern(5 * d))
            .project({42164000, 0, 0}, core::sensor_axes({0, 3000, 0}, {-1, 0, 0}), 1000000);
    ASSERT_FALSE(mesh.sides.empty());
    const auto &point = mesh.sides.front()[1];
    EXPECT_LT(point[0], 6378137.01);
    EXPECT_GT(42164000 - point[0], 30000000);
}
#endif
