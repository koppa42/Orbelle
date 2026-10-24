#include <gtest/gtest.h>
#include <orbelle/render_vsg/labels.hpp>
#include <orbelle/render_vsg/marker_layer.hpp>
#include <orbelle/render_vsg/sensor_layer.hpp>
#include <vsg/all.h>

namespace {
using namespace orbelle;
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
        ASSERT_EQ(a->stateCommands.size(), 3);
        ASSERT_EQ(b->stateCommands.size(), 3);
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
    data->getModifiedCount(previous);
    const auto colors = draw->arrays.at(1)->data.cast<vsg::vec3Array>();
    markers.update(snapshot, std::array<std::uint32_t, 1>{0x123456}, false);
    EXPECT_FALSE(data->differentModifiedCount(previous));
    EXPECT_NEAR(colors->at(0).r, 0x12 / 255.0, 1e-7);
    EXPECT_NEAR(colors->at(0).g, 0x34 / 255.0, 1e-7);
    EXPECT_NEAR(colors->at(0).b, 0x56 / 255.0, 1e-7);
    markers.update(snapshot, std::array<std::uint32_t, 1>{0x123456}, true);
    EXPECT_NEAR(colors->at(0).r, std::pow((0x12 / 255.0 + .055) / 1.055, 2.4), 1e-7);
    EXPECT_THROW(markers.update(snapshot, std::array<std::uint32_t, 1>{0x1000000}),
                 std::invalid_argument);
    snapshot.markers.clear();
    markers.update(snapshot);
    EXPECT_EQ(markers.size(), 0);
}
TEST(DisplaySensor, SurfaceOutlineChordsStayAboveEarthAndScaleWithDpi) {
    visualization::SensorMesh mesh;
    mesh.ground_edges.push_back(
        std::array{core::SensorVector{6378137, 0, 0},
                   core::SensorVector{6378137 * std::cos(.05), 6378137 * std::sin(.05), 0}});
    const std::array meshes{mesh};
    render_vsg::SensorLayer layer(render_vsg::SensorLayer::required(meshes));
    layer.update(meshes, false, 2);
    const auto root = layer.node().cast<vsg::Group>();
    const auto group = root->children.at(1).cast<vsg::StateGroup>();
    const auto draw = group->children.front().cast<vsg::VertexDraw>();
    const auto starts = draw->arrays.at(0)->data.cast<vsg::vec3Array>();
    const auto ends = draw->arrays.at(1)->data.cast<vsg::vec3Array>();
    const auto widths = draw->arrays.at(3)->data.cast<vsg::floatArray>();
    ASSERT_GT(draw->instanceCount, 1u);
    EXPECT_EQ(draw->vertexCount, 6u);
    for (std::uint32_t i = 0; i < draw->instanceCount; ++i) {
        const auto middle = (vsg::dvec3(starts->at(i)) + vsg::dvec3(ends->at(i))) * .5;
        EXPECT_GT(vsg::length(middle), 6378137 + 100);
        EXPECT_NEAR(widths->at(i), 2.4, 1e-6);
        if (i > 0)
            EXPECT_EQ(starts->at(i), ends->at(i - 1));
    }
    layer.update({}, false);
    EXPECT_EQ(draw->instanceCount, 0u);
    EXPECT_THROW(layer.update(meshes, false, 0), std::invalid_argument);
}
TEST(DisplaySensor, SpaceExtensionIsOptionalAndFainterThanGroundedFaces) {
    auto mesh = visualization::SensorView(core::SensorPattern(.785))
                    .project({7e6, 0, 0}, core::sensor_axes({0, 7500, 0}, {-1, 2, 0}), 2e7);
    ASSERT_FALSE(mesh.sides.empty());
    ASSERT_FALSE(mesh.space_sides.empty());
    ASSERT_FALSE(mesh.limb_sides.empty());
    EXPECT_FALSE(mesh.show_space_extension);
    const auto hidden_capacity = render_vsg::SensorLayer::required(std::span(&mesh, 1));
    mesh.show_space_extension = true;
    render_vsg::SensorLayer layer(
        std::max(hidden_capacity, render_vsg::SensorLayer::required(std::span(&mesh, 1))));
    const auto root = layer.node().cast<vsg::Group>();
    const auto group = root->children.front().cast<vsg::StateGroup>();
    const auto draw = group->children.front().cast<vsg::VertexDraw>();
    layer.update(std::span(&mesh, 1), false);
    EXPECT_EQ(draw->vertexCount, (mesh.sides.size() + mesh.space_sides.size()) * 3);
    const auto colors = draw->arrays.at(1)->data.cast<vsg::vec4Array>();
    EXPECT_NEAR(colors->at(0).a, mesh.opacity, 1e-7);
    EXPECT_NEAR(colors->at(mesh.sides.size() * 3).a, mesh.opacity * .25, 1e-7);
    mesh.show_space_extension = false;
    layer.update(std::span(&mesh, 1), false);
    EXPECT_EQ(draw->vertexCount, (mesh.sides.size() + mesh.limb_sides.size()) * 3);
    EXPECT_NEAR(colors->at(mesh.sides.size() * 3).a, mesh.opacity, 1e-7);
    const auto positions = draw->arrays.at(0)->data.cast<vsg::vec3Array>();
    for (std::size_t i = mesh.sides.size() * 3; i < draw->vertexCount; i += 3) {
        // The two lifted rim endpoints and their chord stay outside Earth;
        // otherwise the fragment clip cuts scallops into the filled side.
        const auto middle =
            (vsg::dvec3(positions->at(i + 1)) + vsg::dvec3(positions->at(i + 2))) * .5;
        const auto radius =
            std::sqrt((middle.x * middle.x + middle.y * middle.y) / (6378137. * 6378137.) +
                      middle.z * middle.z / (6356752.314245179 * 6356752.314245179));
        EXPECT_GT(radius, 1 + 100. / 6378137.);
    }
}
TEST(DisplayTrack, AppearanceUpdatesOnlyUniformsAndRejectsInvalidStyle) {
    const std::vector<visualization::RenderSnapshot> samples{
        {core::Epoch{0}, core::FrameId::gcrf, core::OriginId::earth, {{"A", {7e6, 0, 0}}}},
        {core::Epoch{10}, core::FrameId::gcrf, core::OriginId::earth, {{"A", {7e6, 1e3, 0}}}}};
    render_vsg::TrackLayer track(samples), other(samples);
    const auto group = track.node().cast<vsg::StateGroup>();
    const auto original_geometry = group->children.front();
    const auto binding = group->stateCommands[1].cast<vsg::BindDescriptorSet>();
    const auto uniform = binding->descriptorSet->descriptors.front()
                             .cast<vsg::DescriptorBuffer>()
                             ->bufferInfoList.front()
                             ->data;
    vsg::ModifiedCount revision;
    uniform->getModifiedCount(revision);
    track.set_appearance(0xe9a2ba, 4, 3, 2);
    EXPECT_TRUE(uniform->differentModifiedCount(revision));
    uniform->getModifiedCount(revision);
    track.set_appearance(0xe9a2ba, 4, 3, 2);
    EXPECT_FALSE(uniform->differentModifiedCount(revision));
    EXPECT_EQ(group->children.front(), original_geometry);
    EXPECT_NE(group->stateCommands[1], other.node().cast<vsg::StateGroup>()->stateCommands[1]);
    EXPECT_THROW(track.set_appearance(0x1000000, 2, 0), std::invalid_argument);
    EXPECT_THROW(track.set_appearance(0, 0, 0), std::invalid_argument);
    EXPECT_THROW(track.set_appearance(0, 2, 4), std::invalid_argument);
    EXPECT_THROW(track.set_appearance(0, 2, 0, 0), std::invalid_argument);
}
} // namespace
