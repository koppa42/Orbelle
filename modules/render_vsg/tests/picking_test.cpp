#include <gtest/gtest.h>
#include <orbitforge/render_vsg/labels.hpp>
#include <orbitforge/render_vsg/marker_layer.hpp>
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
TEST(DisplayPicking, InertialCameraAndEarthFixedEllipsoidGiveTheSameHits) {
    const core::Epoch epoch{1234};
    core::PositionRotation rotation{epoch,
                                    core::OriginId::earth,
                                    core::FrameId::gcrf,
                                    core::FrameId::itrf,
                                    {{{0, 0, 1}, {0, 1, 0}, {-1, 0, 0}}},
                                    "test",
                                    "test"};
    auto make_camera = [](vsg::dvec3 eye, vsg::dvec3 up) {
        return vsg::Camera::create(vsg::Perspective::create(45, 1280.0 / 720, 1000, 1e8),
                                   vsg::LookAt::create(eye, vsg::dvec3(0, 0, 0), up));
    };
    const visualization::RenderSnapshot fixed{
        epoch,
        core::FrameId::itrf,
        core::OriginId::earth,
        {{"front", {7e6, 0, 0}}, {"back", {-7e6, 0, 0}}, {"polar-limb", {0, 0, 6710e3}}}};
    auto inertial = fixed;
    inertial.frame = core::FrameId::gcrf;
    for (auto &marker : inertial.markers)
        marker.position_m = rotation.inverse_position(marker.position_m);
    auto fixed_camera = make_camera({2e7, 0, 0}, {0, 0, 1});
    auto inertial_camera = make_camera({0, 0, 2e7}, {-1, 0, 0});
    const auto a = render_vsg::visible_markers(fixed, *fixed_camera, 1280, 720);
    const auto b = render_vsg::visible_markers(inertial, *inertial_camera, 1280, 720, 0, &rotation);
    ASSERT_EQ(a.size(), 2u);
    ASSERT_EQ(a.size(), b.size());
    for (std::size_t i = 0; i < a.size(); ++i) {
        EXPECT_EQ(a[i].index, b[i].index);
        EXPECT_NEAR(a[i].x_pixels, b[i].x_pixels, 1e-8);
        EXPECT_NEAR(a[i].y_pixels, b[i].y_pixels, 1e-8);
    }
    rotation.epoch = core::Epoch{1235};
    EXPECT_THROW(
        (void)render_vsg::visible_markers(inertial, *inertial_camera, 1280, 720, 0, &rotation),
        std::invalid_argument);
}
TEST(DisplayTrack, SelectsCachedSegmentsWithoutReplacingGeometry) {
    std::vector<visualization::RenderSnapshot> samples;
    for (int i = 0; i < 5; ++i)
        samples.push_back({core::Epoch{i * 10},
                           core::FrameId::gcrf,
                           core::OriginId::earth,
                           {{"satellite", {7e6, i * 1e3, 0}}}});
    render_vsg::TrackLayer track(samples);
    const auto node = track.node();
    track.set_interval(core::Epoch{15}, core::Epoch{25});
    EXPECT_EQ(track.size(), 3);
    EXPECT_EQ(track.node(), node);
    track.set_interval(core::Epoch{50}, core::Epoch{60});
    EXPECT_EQ(track.size(), 0);
    track.set_interval(core::Epoch{15}, core::Epoch{15});
    EXPECT_EQ(track.size(), 0);
    track.set_interval(core::Epoch{0}, core::Epoch{40});
    EXPECT_EQ(track.size(), 5);
    EXPECT_THROW(track.set_interval(core::Epoch{40}, core::Epoch{0}), std::invalid_argument);
    samples[2].frame = core::FrameId::itrf;
    EXPECT_THROW((void)render_vsg::TrackLayer{samples}, std::invalid_argument);
}
TEST(DisplayTrack, SharesLivePipelineWithoutRetainingItAfterAllLayersClose) {
    const std::vector<visualization::RenderSnapshot> samples{
        {core::Epoch{0}, core::FrameId::gcrf, core::OriginId::earth, {{"A", {7e6, 0, 0}}}},
        {core::Epoch{10}, core::FrameId::gcrf, core::OriginId::earth, {{"A", {7e6, 1e3, 0}}}}};
    vsg::observer_ptr<vsg::StateCommand> pipeline;
    {
        render_vsg::TrackLayer first(samples), second(samples);
        const auto a = first.node().cast<vsg::StateGroup>();
        const auto b = second.node().cast<vsg::StateGroup>();
        ASSERT_EQ(a->stateCommands.size(), 1);
        ASSERT_EQ(b->stateCommands.size(), 1);
        EXPECT_EQ(a->stateCommands.front(), b->stateCommands.front());
        EXPECT_NE(a->children.front(), b->children.front());
        pipeline = a->stateCommands.front();
    }
    EXPECT_FALSE(pipeline.valid());
}
TEST(DisplayMarkers, UnchangedPositionsDoNotScheduleAnotherBufferUpload) {
    render_vsg::MarkerLayer markers(2);
    visualization::RenderSnapshot snapshot{
        core::Epoch{0}, core::FrameId::gcrf, core::OriginId::earth, {{"A", {7e6, 0, 0}}}};
    markers.update(snapshot);
    const auto group = markers.node().cast<vsg::StateGroup>();
    const auto draw = group->children.front().cast<vsg::VertexDraw>();
    const auto data = draw->arrays.front()->data;
    vsg::ModifiedCount previous;
    data->getModifiedCount(previous);
    markers.update(snapshot);
    EXPECT_FALSE(data->differentModifiedCount(previous));
    snapshot.markers.front().position_m[0] += 100;
    markers.update(snapshot);
    EXPECT_TRUE(data->differentModifiedCount(previous));
    snapshot.markers.clear();
    markers.update(snapshot);
    EXPECT_EQ(markers.size(), 0);
}
} // namespace
