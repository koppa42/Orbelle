#include <gtest/gtest.h>
#include <limits>
#include <orbitforge/runtime/demo.hpp>
#include <orbitforge/runtime/ephemeris.hpp>
#include <orbitforge/runtime/time_frames.hpp>
#ifdef ORBITFORGE_HAS_VISUALIZATION
#include <orbitforge/visualization/snapshot.hpp>
#endif

using namespace orbitforge;

TEST(Runtime, EphemerisFactoryMatchesBuildCapability) {
    ASSERT_EQ(runtime::ephemeris_available(), static_cast<bool>(ORBITFORGE_EXPECT_SPICE));
    if (runtime::ephemeris_available()) {
        const auto provider = runtime::load_spice_snapshot(ORBITFORGE_INTEGRATION_SPICE,
                                                           ORBITFORGE_INTEGRATION_WORKSPACE);
        const auto state = provider->geometric_state(301, 399, provider->data_info().first_epoch);
        EXPECT_EQ(state.frame, core::FrameId::spice_j2000);
        EXPECT_EQ(state.velocity_time_scale, core::TimeScale::tdb);
    } else {
        EXPECT_THROW((void)runtime::load_spice_snapshot("missing.json", "unused-workspace"),
                     std::runtime_error);
    }
}

TEST(Runtime, TimeAndFramesMatchBuildCapability) {
    ASSERT_EQ(runtime::time_frames_available(), static_cast<bool>(ORBITFORGE_EXPECT_ERFA));
    if (runtime::time_frames_available()) {
        const auto time = runtime::make_builtin_time_provider();
        const auto epoch = time->from_utc({2000, 1, 1, 11, 59, 28, 0});
        EXPECT_EQ(epoch, core::Epoch(0));
        const auto frames = runtime::make_frame_provider();
        EXPECT_EQ(frames->gcrf_to_itrf(epoch, {epoch, -32, 0, 0, 0, 0, "synthetic"}).to,
                  core::FrameId::itrf);
    } else {
        EXPECT_THROW((void)runtime::make_builtin_time_provider(), std::runtime_error);
        EXPECT_THROW((void)runtime::make_frame_provider(), std::runtime_error);
    }
}

TEST(Runtime, SharedEntryProducesOwnedResultsWithProvenance) {
    const auto result = runtime::make_circular_demo_session().sample(runtime::demo_epochs());
    ASSERT_EQ(result.states().size(), 6U);
    EXPECT_EQ(result.provenance().object_id, "demo-satellite");
    EXPECT_EQ(result.provenance().input_revision, 1U);
    EXPECT_EQ(result.states().back().epoch.tai_nanoseconds(), 300'000'000'000);
}
TEST(Application, RejectsMissingProvider) {
    EXPECT_THROW((application::Session{nullptr, {"object", 1, "model"}}), std::invalid_argument);
}
TEST(Application, UsesInjectedProviderInsteadOfDemoImplementation) {
    class FixedProvider final : public core::IStateProvider {
      public:
        core::CartesianState state_at(core::Epoch epoch) const override {
            return {epoch, core::OriginId::earth, core::FrameId::gcrf, {1, 2, 3}, {4, 5, 6}};
        }
    };
    application::Session session{std::make_shared<FixedProvider>(), {"injected", 7, "test"}};
    const auto result = session.sample(runtime::demo_epochs());
    EXPECT_EQ(result.provenance().input_revision, 7U);
    EXPECT_EQ(result.states().front().position_m[0], 1.0);
}
#ifdef ORBITFORGE_HAS_VISUALIZATION
TEST(Visualization, ExplicitRotationPreservesSourceAndRejectsWrongEpochOrFrame) {
    const visualization::RenderSnapshot source{core::Epoch(12),
                                               core::FrameId::gcrf,
                                               core::OriginId::earth,
                                               {{"a", {2, 3, 4}}, {"b", {-7, 1, 0}}}};
    const core::PositionRotation rotation{core::Epoch(12),
                                          core::OriginId::earth,
                                          core::FrameId::gcrf,
                                          core::FrameId::itrf,
                                          {{{0, -1, 0}, {1, 0, 0}, {0, 0, 1}}},
                                          "quarter-turn-fixture",
                                          "synthetic"};
    const auto result = visualization::rotate_snapshot(source, rotation);
    EXPECT_EQ(result.frame, core::FrameId::itrf);
    EXPECT_EQ(result.epoch, source.epoch);
    EXPECT_EQ(result.markers[0].position_m, (std::array<double, 3>{-3, 2, 4}));
    EXPECT_EQ(result.markers[1].position_m, (std::array<double, 3>{-1, -7, 0}));
    EXPECT_EQ(source.markers[0].position_m, (std::array<double, 3>{2, 3, 4}));
    auto bad = source;
    bad.epoch = core::Epoch(13);
    EXPECT_THROW((void)visualization::rotate_snapshot(bad, rotation), std::invalid_argument);
    bad = source;
    bad.frame = core::FrameId::spice_j2000;
    EXPECT_THROW((void)visualization::rotate_snapshot(bad, rotation), std::invalid_argument);
    bad = source;
    bad.markers[0].position_m[0] = std::numeric_limits<double>::quiet_NaN();
    EXPECT_THROW((void)visualization::rotate_snapshot(bad, rotation), std::invalid_argument);
}
TEST(Visualization, SnapshotOwnsStateAndRejectsMissingSample) {
    const auto series = runtime::make_circular_demo_session().sample(runtime::demo_epochs());
    const auto snapshot = visualization::make_snapshot(series, 0);
    EXPECT_EQ(snapshot.markers.front().object_id, "demo-satellite");
    EXPECT_EQ(snapshot.markers.front().position_m[0], 7e6);
    EXPECT_EQ(snapshot.epoch, core::Epoch{0});
    EXPECT_THROW(static_cast<void>(visualization::make_snapshot(series, 6)), std::out_of_range);
}
#endif
