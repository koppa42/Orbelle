#include <gtest/gtest.h>
#include <orbitforge/render_vsg/labels.hpp>
#include <vsg/all.h>

namespace {
using namespace orbitforge;
TEST(DisplayPicking, RejectsEarthOccludedBehindCameraAndOutsideViewport) {
    auto camera = vsg::Camera::create(
        vsg::Perspective::create(45.0, 1280.0 / 720.0, 1000.0, 1e8),
        vsg::LookAt::create(vsg::dvec3(2e7, 0, 0), vsg::dvec3(0, 0, 0), vsg::dvec3(0, 0, 1)));
    visualization::RenderSnapshot snapshot{
        core::Epoch{0}, core::FrameId::itrf, core::OriginId::earth, {}};
    snapshot.markers = {{"front", {7e6, 0, 0}},
                        {"back", {-7e6, 0, 0}},
                        {"above", {0, 0, 7e6}},
                        {"behind-camera", {3e7, 0, 0}},
                        {"off-screen", {0, 3e7, 0}}};
    const auto visible = render_vsg::visible_markers(snapshot, *camera, 1280, 720);
    ASSERT_EQ(visible.size(), 2u);
    EXPECT_EQ(visible[0].index, 0u);
    EXPECT_DOUBLE_EQ(visible[0].x_pixels, 640.0);
    EXPECT_DOUBLE_EQ(visible[0].y_pixels, 360.0);
    EXPECT_EQ(visible[1].index, 2u);
    EXPECT_LT(visible[1].y_pixels, 360.0); // Vulkan top-left pixel origin.
    snapshot.frame = core::FrameId::gcrf;
    EXPECT_THROW((void)render_vsg::visible_markers(snapshot, *camera, 1280, 720),
                 std::invalid_argument);
}
TEST(DisplayPicking, NearestWithinRadiusAndStableTieBreak) {
    const std::array markers{render_vsg::ProjectedMarker{4, 100, 100},
                             render_vsg::ProjectedMarker{2, 102, 100},
                             render_vsg::ProjectedMarker{1, 120, 120}};
    EXPECT_EQ(render_vsg::pick_marker(markers, 101, 100, 8), 2u);
    EXPECT_EQ(render_vsg::pick_marker(markers, 100, 100, 8), 4u);
    EXPECT_EQ(render_vsg::pick_marker(markers, 92, 100, 8), 4u);
    EXPECT_FALSE(render_vsg::pick_marker(markers, 80, 80, 8));
    EXPECT_THROW((void)render_vsg::pick_marker(markers, 0, 0, 0), std::invalid_argument);
}
TEST(DisplayPicking, AnEyeInsideEarthCannotSeeThroughTheEllipsoid) {
    auto camera = vsg::Camera::create(
        vsg::Perspective::create(45.0, 1280.0 / 720.0, 1000.0, 1e8),
        vsg::LookAt::create(vsg::dvec3(0, 0, 0), vsg::dvec3(1, 0, 0), vsg::dvec3(0, 0, 1)));
    const visualization::RenderSnapshot snapshot{
        core::Epoch{0}, core::FrameId::itrf, core::OriginId::earth, {{"satellite", {7e6, 0, 0}}}};
    EXPECT_TRUE(render_vsg::visible_markers(snapshot, *camera, 1280, 720).empty());
}
} // namespace
