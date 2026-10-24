#include <gtest/gtest.h>
#ifdef ORBELLE_HAS_VISUALIZATION
#include <cmath>
#include <limits>
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
    // Hiding all missed boundary rays must leave a closed, filled Earth-facing volume.
    ASSERT_GE(mesh.limb_sides.size(), 720u);
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
TEST(SensorView, StationTrackingFootprintKeepsItsLimbVertices) {
    const core::SensorVector position{7e6, 0, 0};
    const auto axes = core::sensor_axes(
        {0, 7500, 0}, {6378137 * std::cos(20 * d) - position[0], 6378137 * std::sin(20 * d), 0});
    const core::SensorPattern pattern(45 * d);
    const core::SensorGeometry geometry(pattern);
    const auto mesh = visualization::SensorView(pattern).project(position, axes, 2e7);
    ASSERT_FALSE(mesh.ground.empty());
    std::size_t limb_vertices = 0;
    double maximum_gap = 0;
    for (const auto &edge : mesh.ground_edges) {
        for (const auto &point : edge) {
            const core::SensorVector ray{point[0] - position[0], point[1], point[2]};
            if (geometry.margin(axes.to_local(ray)) < .01)
                continue;
            ++limb_vertices;
            double nearest = std::numeric_limits<double>::infinity();
            for (const auto &triangle : mesh.ground)
                for (const auto &vertex : triangle)
                    nearest =
                        std::min(nearest, std::hypot(point[0] - vertex[0], point[1] - vertex[1],
                                                     point[2] - vertex[2]));
            maximum_gap = std::max(maximum_gap, nearest);
        }
    }
    ASSERT_GT(limb_vertices, 20u);
    // The footprint fill and its independently generated tangent outline must meet.
    // Five metres allows quadratic round-off at tangency, not missing facets.
    EXPECT_LT(maximum_gap, 5);
}
TEST(SensorView, StationTrackingDoesNotBridgeGroundAndSpaceEndpoints) {
    const core::SensorVector position{7e6, 0, 0};
    const auto axes = core::sensor_axes(
        {0, 7500, 0}, {6378137 * std::cos(20 * d) - position[0], 6378137 * std::sin(20 * d), 0});
    const auto mesh =
        visualization::SensorView(core::SensorPattern(45 * d)).project(position, axes, 2e7);
    std::size_t ground_facets = 0, space_facets = 0;
    for (const auto &triangle : mesh.sides) {
        const auto on_surface = [](const auto &p) {
            const auto q = (p[0] * p[0] + p[1] * p[1]) / (6378137. * 6378137.) +
                           p[2] * p[2] / (6356752.314245179 * 6356752.314245179);
            return std::abs(q - 1) < 1e-10;
        };
        const bool first = on_surface(triangle[1]), second = on_surface(triangle[2]);
        EXPECT_EQ(first, second) << "A side facet crosses the discontinuity at the Earth limb";
        if (first && second)
            ++ground_facets;
        EXPECT_TRUE(first && second);
    }
    for (const auto &triangle : mesh.space_sides) {
        for (int i = 1; i <= 2; ++i)
            EXPECT_NEAR(std::hypot(triangle[i][0] - position[0], triangle[i][1], triangle[i][2]),
                        2e7, 1e-7);
        ++space_facets;
    }
    EXPECT_GT(ground_facets, 0u);
    EXPECT_GT(space_facets, 0u);
    ASSERT_FALSE(mesh.limb_sides.empty());
    const auto scaled_dot = [](const auto &x, const auto &y) {
        return (x[0] * y[0] + x[1] * y[1]) / (6378137. * 6378137.) +
               x[2] * y[2] / (6356752.314245179 * 6356752.314245179);
    };
    for (const auto &triangle : mesh.limb_sides) {
        EXPECT_EQ(triangle[0], position);
        for (int i = 1; i <= 2; ++i) {
            EXPECT_NEAR(scaled_dot(triangle[i], triangle[i]), 1, 1e-12);
            // Tangent plane to the ellipsoid contains the satellite.
            EXPECT_NEAR(scaled_dot(position, triangle[i]), 1, 1e-12);
        }
    }
}
#endif
