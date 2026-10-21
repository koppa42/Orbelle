#include <gtest/gtest.h>
#include <limits>
#include <orbitforge/runtime/demo.hpp>
#include <orbitforge/runtime/ephemeris.hpp>
#include <orbitforge/runtime/time_frames.hpp>
#include <orbitforge/runtime/two_body.hpp>
#ifdef ORBITFORGE_HAS_VISUALIZATION
#include <orbitforge/visualization/snapshot.hpp>
#include <orbitforge/visualization/state_track.hpp>
#endif

using namespace orbitforge;

TEST(Runtime, TwoBodyFactorySharesApplicationOwnershipAndBuildCapability) {
    EXPECT_EQ(runtime::two_body_available(), static_cast<bool>(ORBITFORGE_EXPECT_SPICE));
    const core::CartesianState initial{
        core::Epoch{0}, core::OriginId::earth, core::FrameId::gcrf, {3, 0, 0}, {0, 2, 0}};
    if (!runtime::two_body_available()) {
        EXPECT_THROW((void)runtime::make_two_body_session(initial, core::GravitationalParameter{6},
                                                          "unavailable"),
                     std::runtime_error);
        return;
    }
    const auto result =
        runtime::make_two_body_session(initial, core::GravitationalParameter{6}, "parabolic", 7)
            .sample(std::array{core::Epoch{4'000'000'000}});
    EXPECT_EQ(result.provenance().object_id, "parabolic");
    EXPECT_EQ(result.provenance().input_revision, 7U);
    EXPECT_EQ(result.provenance().algorithm, "two-body-cspice-n0067-prop2b-tai-v1");
    EXPECT_NEAR(result.states().front().position_m[1], 6, 1e-8);
    EXPECT_NEAR(result.states().front().velocity_mps[0], -1, 1e-8);
}

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
TEST(Visualization, HermiteDisplayTrackReproducesCubicAndDoesNotExtrapolate) {
    // Non-uniform intervals at a large absolute epoch, with exact cubic derivatives.
    constexpr std::int64_t base = 8'000'000'000'000'000'000;
    std::vector<core::CartesianState> states;
    for (const auto t : {0, 2, 7})
        states.push_back({core::Epoch{base + t * 1'000'000'000LL},
                          core::OriginId::earth,
                          core::FrameId::itrf,
                          {double(t * t * t), double(t * t), double(t)},
                          {double(3 * t * t), double(2 * t), 1}});
    const visualization::StateTrack track{results::StateSeries{{"test", 9, "cubic"}, states}};
    states.front().position_m[0] = 500;
    EXPECT_EQ(track.nodes().front().markers.front().position_m[0], 0);
    for (const auto ns :
         {0LL, 1LL, 999'999'999LL, 2'000'000'000LL, 3'456'789'012LL, 7'000'000'000LL}) {
        const core::Epoch epoch{base + ns};
        const auto sample = track.sample(epoch);
        const double t = static_cast<double>(ns) * 1e-9;
        EXPECT_EQ(sample.epoch, epoch);
        EXPECT_EQ(sample.frame, core::FrameId::itrf);
        EXPECT_EQ(sample.markers.front().object_id, "test");
        EXPECT_NEAR(sample.markers.front().position_m[0], t * t * t, 1e-10);
        EXPECT_NEAR(sample.markers.front().position_m[1], t * t, 1e-10);
        EXPECT_NEAR(sample.markers.front().position_m[2], t, 1e-10);
        const auto state = track.sample_state(epoch);
        EXPECT_NEAR(state.velocity_mps[0], 3 * t * t, 1e-10);
        EXPECT_NEAR(state.velocity_mps[1], 2 * t, 1e-10);
        EXPECT_NEAR(state.velocity_mps[2], 1, 1e-10);
    }
    EXPECT_THROW((void)track.sample(core::Epoch{base - 1}), std::out_of_range);
    EXPECT_THROW((void)track.sample(core::Epoch{base + 7'000'000'001}), std::out_of_range);
    states.back().epoch = states.front().epoch;
    EXPECT_THROW((visualization::StateTrack{results::StateSeries{{"bad", 1, "test"}, states}}),
                 std::invalid_argument);
}

TEST(Visualization, DisplayTrackRejectsMixedAxesAndNonfiniteTangents) {
    const core::CartesianState a{
        core::Epoch{0}, core::OriginId::earth, core::FrameId::itrf, {1, 2, 3}, {4, 5, 6}};
    auto b = a;
    b.epoch = core::Epoch{1'000'000'000};
    b.frame = core::FrameId::gcrf;
    EXPECT_THROW((visualization::StateTrack{results::StateSeries{{"bad", 1, "test"}, {a, b}}}),
                 std::invalid_argument);
    b.frame = a.frame;
    b.velocity_mps[2] = std::numeric_limits<double>::infinity();
    EXPECT_THROW((visualization::StateTrack{results::StateSeries{{"bad", 1, "test"}, {a, b}}}),
                 std::invalid_argument);
}

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
