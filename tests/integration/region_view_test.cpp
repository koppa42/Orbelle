#include <gtest/gtest.h>
#ifdef ORBELLE_HAS_VISUALIZATION
#include <algorithm>
#include <cmath>
#include <numbers>
#include <orbelle/visualization/region_view.hpp>
using namespace orbelle;
TEST(RegionView, RectangleFillCoversWholeWorldAndRemainsAboveEllipsoid) {
    core::RegionBoundary region;
    region.south_rad = -std::numbers::pi / 2;
    region.north_rad = std::numbers::pi / 2;
    region.west_rad = -std::numbers::pi;
    region.east_rad = std::numbers::pi;
    const auto mesh = visualization::region_mesh(region);
    ASSERT_FALSE(mesh.triangles.empty());
    EXPECT_LE(mesh.triangles.size(), 130000);
    double min_x = 1e10, max_x = -1e10, min_z = 1e10, max_z = -1e10;
    for (const auto &triangle : mesh.triangles) {
        std::array<double, 3> centre{};
        for (const auto &p : triangle)
            for (std::size_t i = 0; i < 3; ++i)
                centre[i] += p[i] / 3;
        const auto location =
            core::ground_station_location(core::GroundPositionType::cartesian, centre);
        EXPECT_GT(location.height_m, 400);
        min_x = std::min(min_x, centre[0]);
        max_x = std::max(max_x, centre[0]);
        min_z = std::min(min_z, centre[2]);
        max_z = std::max(max_z, centre[2]);
    }
    EXPECT_LT(min_x, -6e6);
    EXPECT_GT(max_x, 6e6);
    EXPECT_LT(min_z, -6e6);
    EXPECT_GT(max_z, 6e6);
    const auto outline = visualization::region_mesh(region, false);
    EXPECT_EQ(outline.outline, mesh.outline);
    EXPECT_TRUE(outline.triangles.empty());
    region.south_rad = -.1;
    region.north_rad = .2;
    region.west_rad = 3;
    region.east_rad = -3;
    const auto seam = visualization::region_mesh(region);
    ASSERT_FALSE(seam.triangles.empty());
    for (const auto &triangle : seam.triangles)
        for (const auto &p : triangle)
            EXPECT_LT(p[0], -6e6);
}
TEST(RegionView, ConcaveDateLineAndReversedPolygonsDoNotFillTheirNotch) {
    core::RegionBoundary region;
    region.vertices = {{0, 0}, {.3, 0}, {.3, .3}, {.15, .12}, {0, .3}};
    for (bool seam : {false, true})
        for (bool reverse : {false, true}) {
            auto shape = region;
            if (seam)
                for (auto &p : shape.vertices)
                    p.longitude_rad = std::remainder(p.longitude_rad + 3.0, 2 * std::numbers::pi);
            if (reverse)
                std::ranges::reverse(shape.vertices);
            const auto mesh = visualization::region_mesh(shape);
            ASSERT_FALSE(mesh.triangles.empty());
            EXPECT_LT(mesh.triangles.size(), 10000);
            for (const auto &triangle : mesh.triangles) {
                std::array<double, 3> centre{};
                for (const auto &p : triangle)
                    for (std::size_t i = 0; i < 3; ++i)
                        centre[i] += p[i] / 3;
                const auto location =
                    core::ground_station_location(core::GroundPositionType::cartesian, centre);
                ASSERT_TRUE(
                    core::region_contains(shape, {location.longitude_rad, location.latitude_rad}));
                EXPECT_GT(location.height_m, 500);
            }
        }
}
TEST(RegionView, CollinearVerticesAndHighLatitudeRemainValidDisplayGeometry) {
    for (const std::vector<core::RegionVertex> vertices :
         {std::vector<core::RegionVertex>{{0, 0}, {.1, 0}, {.2, 0}, {.3, 0}, {.3, .2}, {0, .2}},
          std::vector<core::RegionVertex>{{-1, 1.3}, {1, 1.3}, {0, 1.5}}}) {
        core::RegionBoundary boundary;
        boundary.vertices = vertices;
        const auto mesh = visualization::region_mesh(boundary);
        ASSERT_FALSE(mesh.triangles.empty());
        for (const auto &triangle : mesh.triangles) {
            std::array<double, 3> centre{};
            for (const auto &p : triangle)
                for (std::size_t i = 0; i < 3; ++i)
                    centre[i] += p[i] / 3;
            const auto location =
                core::ground_station_location(core::GroundPositionType::cartesian, centre);
            ASSERT_TRUE(
                core::region_contains(boundary, {location.longitude_rad, location.latitude_rad}));
            EXPECT_GT(location.height_m, 500);
        }
    }
}
#endif
