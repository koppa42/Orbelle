#include <gtest/gtest.h>
#include <orbitforge/orbitforge.hpp>
#include <orbitforge/runtime/access.hpp>
#include <orbitforge/runtime/hpop.hpp>
#include <orbitforge/runtime/time_frames.hpp>

using namespace orbitforge;
namespace {
application::Project hpop_project() {
    application::Project project{"Numerical", core::Epoch{0}, core::Epoch{7200000000000}};
    application::ProjectSatellite satellite;
    satellite.orbit.propagator = application::OrbitPropagator::hpop;
    satellite.orbit.hpop.gravity_degree = satellite.orbit.hpop.gravity_order = 0;
    project.satellites.push_back(satellite);
    return project;
}
} // namespace
TEST(HpopProject, OptionsPersistAndInvalidateAnalysisOnlyWhenActive) {
    auto input = hpop_project();
    input.satellites[0].orbit.hpop.max_step_seconds = 25;
    const auto restored = application::decode_project(application::encode_project(input));
    EXPECT_EQ(restored.satellites[0].orbit.hpop, input.satellites[0].orbit.hpop);
    const auto hash = application::analysis_input_sha256(input);
    const auto trajectory_hash = [&] {
        const auto project = Project::from_data(input);
        return project.prepare()
            .states(project.objects().get("satellite-1"), std::array{input.first}, FrameId::gcrf)
            .input_sha256();
    };
    const auto state_hash = trajectory_hash();
    input.satellites[0].orbit.hpop.fixed_step_seconds = 12; // inactive RK4 field
    EXPECT_EQ(application::analysis_input_sha256(input), hash);
    EXPECT_EQ(trajectory_hash(), state_hash);
    input.satellites[0].orbit.step_seconds = 5; // output interval
    EXPECT_EQ(application::analysis_input_sha256(input), hash);
    input.satellites[0].orbit.hpop.max_step_seconds = 30;
    EXPECT_NE(application::analysis_input_sha256(input), hash);
    EXPECT_NE(trajectory_hash(), state_hash);
    input.satellites[0].orbit.hpop.gravity_order = 1;
    EXPECT_THROW(application::validate_project(input), application::ProjectError);
}
TEST(HpopProject, LongStepRangePersistsAndUsesDistinctAdaptiveAlgorithmIdentity) {
    auto input = hpop_project();
    auto &options = input.satellites[0].orbit.hpop;
    options.min_step_seconds = options.max_step_seconds = options.fixed_step_seconds = 86400;
    const auto restored = application::decode_project(application::encode_project(input));
    EXPECT_EQ(restored.satellites[0].orbit.hpop, options);
    const auto identity = runtime::report_input_sha256(input);
    options.max_step_seconds = options.min_step_seconds = 43200;
    EXPECT_NE(runtime::report_input_sha256(input), identity);
    options.step_control = core::HpopStepControl::fixed;
    const auto fixed_identity = runtime::report_input_sha256(input);
    options.max_step_seconds = options.min_step_seconds = 86400;
    EXPECT_EQ(runtime::report_input_sha256(input), fixed_identity);
    options.fixed_step_seconds = 43200;
    EXPECT_NE(runtime::report_input_sha256(input), fixed_identity);
    for (auto method : {core::HpopIntegrator::rkf78, core::HpopIntegrator::rkv89,
                        core::HpopIntegrator::bulirsch_stoer}) {
        options.integrator = method;
        options.step_control = core::HpopStepControl::relative_error;
        const auto session = runtime::make_hpop_session(input.satellites[0].orbit.initial, options);
        EXPECT_NE(session.provenance().algorithm.find("steps=relative-mixed-max-span-v2"),
                  std::string::npos);
    }
}
TEST(HpopProject, MassPersistsButOnlyAreaMassRatiosChangeSphericalCalculationIdentity) {
    auto input = hpop_project();
    auto &options = input.satellites[0].orbit.hpop;
    for (bool active : {false, true}) {
        options.drag.enabled = options.srp.enabled = active;
        const auto hash = application::analysis_input_sha256(input);
        const auto report_hash = runtime::report_input_sha256(input);
        const auto encoded = application::encode_project(input);
        options.satellite_mass_kg *= 2;
        EXPECT_EQ(application::analysis_input_sha256(input), hash);
        EXPECT_EQ(runtime::report_input_sha256(input), report_hash);
        EXPECT_NE(application::encode_project(input), encoded);
        EXPECT_EQ(application::decode_project(application::encode_project(input))
                      .satellites[0]
                      .orbit.hpop.satellite_mass_kg,
                  options.satellite_mass_kg);
        options.drag.area_mass_ratio_m2_per_kg *= 2;
        if (active) {
            EXPECT_NE(application::analysis_input_sha256(input), hash);
            EXPECT_NE(runtime::report_input_sha256(input), report_hash);
        } else {
            EXPECT_EQ(application::analysis_input_sha256(input), hash);
            EXPECT_EQ(runtime::report_input_sha256(input), report_hash);
        }
    }
    options.satellite_mass_kg = 0;
    EXPECT_THROW((void)runtime::report_input_sha256(input), std::invalid_argument);
}
TEST(HpopProject, PreparedSessionsDriveReportsAccessAndRetainForceDependencies) {
    if (!runtime::time_frames_available()) {
        GTEST_SKIP();
    }
    auto input = hpop_project();
    input.satellites[0].orbit.hpop = {};
    input.ground_stations.push_back({"station", "Equator", {0, 0, 0}});
    auto project = Project::from_data(input);
    const auto data = runtime::DataSnapshot::load(ORBITFORGE_REPORT_EOP);
    input.earth_orientation = data.eop()->data_info();
    const auto session =
        runtime::make_hpop_session(input.satellites[0].orbit.initial,
                                   input.satellites[0].orbit.hpop, {data.frames(), data.eop()});
    EXPECT_NE(session.provenance().algorithm.find(data.data_lock().sha256()), std::string::npos);
    runtime::AccessSession direct{
        {input, data.time(), data.frames(), data.eop()}, "station", "satellite-1"};
    int integration_checks = 0;
    EXPECT_THROW((void)direct.at(input.last,
                                 [&] {
                                     if (++integration_checks > 5)
                                         throw runtime::AnalysisCancelled{};
                                 }),
                 runtime::AnalysisCancelled);
    EXPECT_GT(integration_checks, 5);
    EXPECT_NO_THROW((void)direct.at(input.first));
    const auto analysis = project.prepare(data);
    const auto satellite = project.objects().get("satellite-1");
    const auto times =
        std::array{core::Epoch{600000000000}, core::Epoch{0}, core::Epoch{600000000000}};
    const auto states = analysis.states(satellite, times, core::FrameId::gcrf);
    EXPECT_EQ(states.states()[0].position_m, states.states()[2].position_m);
    EXPECT_EQ(states.data_sha256(), data.data_lock().sha256());
    const auto report = analysis.report({.kind = ReportKind::cartesian_state,
                                         .object_ids = {satellite.id()},
                                         .frame = FrameId::gcrf},
                                        {.columns = {}, .time_scale = ReportTimeScale::tai});
    EXPECT_FALSE(report.table.rows().empty());
    const auto station = project.objects().get("station");
    EXPECT_EQ(analysis.aer(station, satellite, times).samples().size(), 3U);
    int calls = 0;
    EXPECT_THROW((void)analysis.report({.kind = ReportKind::cartesian_state,
                                        .object_ids = {satellite.id()},
                                        .frame = FrameId::gcrf},
                                       {.columns = {}, .time_scale = ReportTimeScale::tai},
                                       [&] { return ++calls > 5; }),
                 runtime::AnalysisCancelled);
    EXPECT_NO_THROW((void)analysis.state(satellite, core::Epoch{0}, core::FrameId::gcrf));
}
TEST(HpopProject, RejectsMissingDataAndInitialEpochOutsideCoverageBeforeQueries) {
    auto input = hpop_project();
    input.satellites[0].orbit.hpop = {};
    EXPECT_THROW((void)Project::from_data(input).prepare(), std::invalid_argument);
    if (!runtime::time_frames_available()) {
        GTEST_SKIP();
    }
    const auto data = runtime::DataSnapshot::load(ORBITFORGE_REPORT_EOP);
    input.satellites[0].orbit.initial.epoch = core::Epoch{-864000000000000};
    const auto interval = application::calculation_data_interval(input);
    EXPECT_EQ(interval.first, input.satellites[0].orbit.initial.epoch);
    EXPECT_EQ(interval.second, input.last);
    EXPECT_THROW((void)Project::from_data(input).prepare(data), core::EopCoverageError);
}
