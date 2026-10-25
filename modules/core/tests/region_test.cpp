#include <algorithm>
#include <gtest/gtest.h>
#include <limits>
#include <orbelle/core/propagation/region.hpp>

using namespace orbelle::core;
namespace {
constexpr double degree = std::numbers::pi / 180;
RegionVertex vertex(double longitude, double latitude) {
    return {longitude * degree, latitude * degree};
}
RegionBoundary triangle() {
    RegionBoundary r;
    r.vertices = {vertex(0, 0), vertex(10, 0), vertex(0, 10)};
    return r;
}
} // namespace
TEST(Region, RectangleUsesExistingWeightedGridAndGlobalLongitude) {
    RegionBoundary r;
    r.south_rad = -std::numbers::pi / 2;
    r.north_rad = std::numbers::pi / 2;
    r.west_rad = -std::numbers::pi;
    r.east_rad = std::numbers::pi;
    EXPECT_TRUE(region_contains(r, vertex(180, 90)));
    EXPECT_TRUE(region_contains(r, vertex(-180, -90)));
    const auto g = region_grid_bounds(r);
    const auto cells = region_coverage_grid(r);
    const auto expected = coverage_grid(g);
    ASSERT_EQ(cells.size(), expected.size());
    for (std::size_t i = 0; i < cells.size(); ++i)
        EXPECT_DOUBLE_EQ(cells[i].area_m2, expected[i].area_m2);
}
TEST(Region, OneDegreeGlobalGridRetainsWgs84AreaAndBoundedSize) {
    RegionBoundary r;
    r.south_rad = -std::numbers::pi / 2;
    r.north_rad = std::numbers::pi / 2;
    r.west_rad = -std::numbers::pi;
    r.east_rad = std::numbers::pi;
    CoverageGrid settings;
    settings.resolution_rad = degree;
    const auto cells = region_coverage_grid(r, settings);
    EXPECT_GT(cells.size(), 40000);
    EXPECT_LT(cells.size(), 43000);
    double area = 0, correction = 0;
    for (const auto &cell : cells) {
        const auto value = cell.area_m2 - correction;
        const auto next = area + value;
        correction = (next - area) - value;
        area = next;
    }
    EXPECT_NEAR(area, 510065621724088.5, 20);
    settings.resolution_rad = .5 * degree;
    const auto finer = region_coverage_grid(r, settings);
    EXPECT_GT(finer.size(), 160000);
    EXPECT_EQ(finer.size(), coverage_grid_size(region_grid_bounds(r, settings)));
    settings.resolution_rad = .2 * degree;
    EXPECT_GT(coverage_grid_size(region_grid_bounds(r, settings)), 1000000);
    EXPECT_THROW((void)coverage_grid_size(region_grid_bounds(r, settings), 65536),
                 std::invalid_argument);
    settings.resolution_rad = std::numeric_limits<double>::denorm_min();
    EXPECT_THROW((void)coverage_grid_size(region_grid_bounds(r, settings)), std::invalid_argument);
}

TEST(Region, StreamingPolygonMatchesMaterializedGridAndCanCancelOutsideRegion) {
    const auto r = triangle();
    CoverageGrid settings;
    settings.resolution_rad = degree;
    const auto expected = region_coverage_grid(r, settings);
    std::size_t count = 0;
    visit_region_coverage_grid(r, settings, [&](const auto &cell) {
        ASSERT_LT(count, expected.size());
        EXPECT_DOUBLE_EQ(cell.area_m2, expected[count].area_m2);
        EXPECT_DOUBLE_EQ(cell.location.longitude_rad, expected[count].location.longitude_rad);
        ++count;
    });
    EXPECT_EQ(count, expected.size());
    settings.resolution_rad = 1e-6;
    EXPECT_THROW(
        visit_region_coverage_grid(
            r, settings, [](const auto &) {}, [] { throw std::runtime_error("cancelled"); }),
        std::runtime_error);
}

TEST(Region, SphericalTriangleMembershipAndWindingAreIndependentOfVertexOrder) {
    auto r = triangle();
    EXPECT_TRUE(region_contains(r, vertex(2, 2)));
    EXPECT_TRUE(region_contains(r, vertex(5, 0)));
    EXPECT_FALSE(region_contains(r, vertex(8, 8)));
    EXPECT_FALSE(region_contains(r, vertex(-178, -2)));
    CoverageGrid settings;
    settings.resolution_rad = degree;
    const auto before = region_coverage_grid(r, settings);
    std::ranges::reverse(r.vertices);
    const auto after = region_coverage_grid(r, settings);
    ASSERT_EQ(before.size(), after.size());
    for (std::size_t i = 0; i < before.size(); ++i) {
        EXPECT_NEAR(before[i].location.longitude_rad, after[i].location.longitude_rad, 1e-14);
        EXPECT_NEAR(before[i].location.latitude_rad, after[i].location.latitude_rad, 1e-14);
        EXPECT_NEAR(before[i].area_m2, after[i].area_m2, 1e-3);
    }
}
TEST(Region, ConcaveBoundaryDoesNotFillItsBoundingRectangle) {
    RegionBoundary r;
    r.vertices = {vertex(0, 0), vertex(10, 0), vertex(10, 2),
                  vertex(2, 2), vertex(2, 10), vertex(0, 10)};
    EXPECT_TRUE(region_contains(r, vertex(1, 8)));
    EXPECT_TRUE(region_contains(r, vertex(8, 1)));
    EXPECT_FALSE(region_contains(r, vertex(8, 8)));
    CoverageGrid settings;
    settings.resolution_rad = degree;
    EXPECT_LT(region_coverage_grid(r, settings).size(),
              coverage_grid(region_grid_bounds(r, settings)).size());
}
TEST(Region, AntimeridianAndGreatCircleLatitudeExtrema) {
    RegionBoundary r;
    r.vertices = {vertex(170, -5), vertex(-170, -5), vertex(-170, 5), vertex(170, 5)};
    const auto bounds = region_grid_bounds(r);
    EXPECT_GT(bounds.west_rad, bounds.east_rad);
    EXPECT_GT(bounds.north_rad, 5 * degree);
    EXPECT_TRUE(region_contains(r, vertex(179, 0)));
    EXPECT_TRUE(region_contains(r, vertex(-179, 0)));
    EXPECT_FALSE(region_contains(r, vertex(0, 0)));
    const auto outline = region_outline(r);
    EXPECT_EQ(outline.front(), outline.back());
    for (std::size_t i = 1; i < outline.size(); ++i)
        EXPECT_LT(std::abs(outline[i].longitude_rad - outline[i - 1].longitude_rad), degree);
}
TEST(Region, InvalidAndSelfIntersectingPolygonsAreRejected) {
    auto r = triangle();
    r.vertices.push_back(r.vertices.front());
    EXPECT_THROW(validate_region_boundary(r), std::invalid_argument);
    r.vertices = {vertex(0, 0), vertex(10, 10), vertex(0, 10), vertex(10, 0)};
    EXPECT_THROW(validate_region_boundary(r), std::invalid_argument);
    r.vertices = {vertex(0, 0), vertex(5, 0), vertex(10, 0)};
    EXPECT_THROW(validate_region_boundary(r), std::invalid_argument);
    r.vertices = {vertex(0, 90), vertex(5, 0), vertex(10, 0)};
    EXPECT_THROW(validate_region_boundary(r), std::invalid_argument);
    r = triangle();
    r.vertices[1].latitude_rad = std::numeric_limits<double>::quiet_NaN();
    EXPECT_THROW(validate_region_boundary(r), std::invalid_argument);
}
