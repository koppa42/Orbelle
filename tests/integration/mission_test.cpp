#include <gtest/gtest.h>
#include <orbitforge/orbitforge.hpp>
#include <orbitforge/runtime/report.hpp>

using namespace orbitforge;
using namespace std::chrono_literals;
TEST(MissionIntegration, RoundTripHashInvalidPlanAndImmutableAnalysis) {
    Project project{"Mission", Epoch{0}, Epoch{600'000'000'000}};
    OrbitInput input;
    input.propagator = OrbitPropagator::mission;
    input.mission = {MissionCoastModel::hpop,
                     {MissionSegment::coast(123'000'000'001ns, "Wait"),
                      MissionSegment::impulse({50, -2, 1}, ManeuverFrame::vnc, "Raise"),
                      MissionSegment::coast(476'999'999'999ns)}};
    input.hpop.gravity_degree = input.hpop.gravity_order = 0;
    const auto satellite =
        project.objects().create(objects::Satellite{.name = "Maneuvering", .orbit = input});
    const auto analysis = project.prepare(DataSnapshot{});
    const auto final = analysis.state(satellite, project.last(), FrameId::gcrf);
    const auto events = analysis.maneuvers(satellite);
    ASSERT_EQ(events.events().size(), 1U);
    EXPECT_EQ(events.events()[0].before.epoch, Epoch{123'000'000'001});
    EXPECT_EQ(events.events()[0].before.position_m, events.events()[0].after.position_m);
    EXPECT_EQ(events.input_sha256(), analysis.states(satellite, 60s, FrameId::gcrf).input_sha256());
    EXPECT_THROW((void)analysis.maneuvers(satellite, [] { return true; }),
                 runtime::AnalysisCancelled);
    auto data = project.to_data();
    const auto bytes = application::encode_project(data);
    auto restored = application::decode_project(bytes);
    EXPECT_EQ(application::encode_project(restored), bytes);
    EXPECT_EQ(restored.satellites[0].orbit.mission, input.mission);
    const auto physical_hash = application::analysis_input_sha256(data);
    const auto report_hash = runtime::report_input_sha256(data);
    data.satellites[0].orbit.mission.segments[1].name = "Presentation only";
    EXPECT_EQ(application::analysis_input_sha256(data), physical_hash);
    EXPECT_EQ(runtime::report_input_sha256(data), report_hash);
    data.satellites[0].orbit.mission.segments[1].delta_v_mps[0] = 51;
    EXPECT_NE(application::analysis_input_sha256(data), physical_hash);
    EXPECT_NE(runtime::report_input_sha256(data), report_hash);
    data.satellites[0].orbit.mission.segments[2].duration_nanoseconds = 1;
    EXPECT_THROW(application::validate_project(data), application::ProjectError);
    EXPECT_EQ(analysis.state(satellite, project.last(), FrameId::gcrf).position_m,
              final.position_m);
    auto unsupported = bytes;
    unsupported[8] = 13;
    EXPECT_THROW((void)application::decode_project(unsupported), application::ProjectError);
}
TEST(MissionIntegration, NoncentralCoastsRequireEopAndIncludeInitialEpochInCoverage) {
    application::Project data{"Coverage", Epoch{0}, Epoch{600'000'000'000}};
    application::ProjectSatellite satellite;
    auto &orbit = satellite.orbit;
    orbit.propagator = OrbitPropagator::mission;
    orbit.initial.epoch = Epoch{-600'000'000'000};
    orbit.first = data.first;
    orbit.last = data.last;
    orbit.mission = {MissionCoastModel::hpop, {MissionSegment::coast(1200s)}};
    data.satellites.push_back(satellite);
    EXPECT_EQ(application::calculation_data_interval(data).first, orbit.initial.epoch);
    auto project = Project::from_data(data);
    EXPECT_THROW((void)project.prepare(DataSnapshot{}), std::invalid_argument);
}
