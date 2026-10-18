#include <cmath>
#include <gtest/gtest.h>
#include <limits>
#include <orbitforge/runtime/analysis.hpp>
#include <orbitforge/runtime/project.hpp>
#include <orbitforge/runtime/time_frames.hpp>

using namespace orbitforge;
namespace app = orbitforge::application;
namespace {
app::Project example() {
    app::Project p{"Covariance", core::Epoch{-60000000000}, core::Epoch{60000000000}};
    app::ProjectSatellite satellite;
    auto &o = satellite.orbit;
    o.propagator = app::OrbitPropagator::hpop;
    o.first = p.first;
    o.last = p.last;
    o.hpop.gravity_degree = o.hpop.gravity_order = 0;
    o.hpop.relative_tolerance = 1e-13;
    o.hpop.position_tolerance_m = 1e-6;
    o.hpop.velocity_tolerance_mps = 1e-9;
    auto &s = o.covariance;
    s.enabled = s.consider_analysis = s.drag_enabled = s.srp_enabled = true;
    s.consider_cross_correlation = true;
    for (std::size_t i = 0; i < 6; ++i)
        s.state[i][i] = i < 3 ? 10000 : 1;
    s.state[0][1] = s.state[1][0] = 1.25;
    s.drag = {1e-4, {.1, 0, 0, 0, 0, 0}};
    s.srp = {2e-4, {0, -.1, 0, 0, 0, 0}};
    s.drag_srp_covariance = 5e-5;
    p.satellites.push_back(satellite);
    return p;
}
app::ReportDefinition report() {
    app::ReportDefinition d;
    d.id = "covariance";
    d.name = "Covariance";
    d.query.kind = app::ReportKind::state_covariance;
    d.query.scope = app::ReportScope::all_compatible;
    d.query.step_nanoseconds = 60000000000;
    d.format.time_scale = app::ReportTimeScale::tai;
    return d;
}
} // namespace
TEST(HpopCovarianceProject, FullConfigurationAndReportRoundTripExactly) {
    auto p = example();
    p.reports.push_back(report());
    const auto bytes = app::encode_project(p);
    const auto restored = app::decode_project(bytes);
    EXPECT_EQ(restored.satellites[0].orbit.covariance, p.satellites[0].orbit.covariance);
    EXPECT_EQ(app::encode_project(restored), bytes);
    auto &s = p.satellites[0].orbit.covariance;
    s.enabled = false;
    s.coordinates = {core::HpopCovarianceFrame::true_of_date,
                     core::HpopCovarianceVelocity::rotating};
    s.independent_gravity = true;
    s.gravity = {2190, 2159};
    s.consider_analysis = false;
    EXPECT_EQ(app::decode_project(app::encode_project(p)).satellites[0].orbit.covariance, s);
}
TEST(HpopCovarianceProject, DisabledDraftsDoNotChangeIdentityAndActiveEditsDo) {
    auto p = example();
    auto &s = p.satellites[0].orbit.covariance;
    s.enabled = false;
    const auto hash = app::analysis_input_sha256(p), report_hash = runtime::report_input_sha256(p);
    s.state[0][0] *= 2;
    s.coordinates.frame = core::HpopCovarianceFrame::lvlh;
    s.gravity = {200, 150};
    s.drag.variance *= 2;
    EXPECT_EQ(app::analysis_input_sha256(p), hash);
    EXPECT_EQ(runtime::report_input_sha256(p), report_hash);
    s.enabled = true;
    EXPECT_NE(app::analysis_input_sha256(p), hash);
    const auto active = app::analysis_input_sha256(p);
    s.gravity = {100, 25}; // Independent truncation is disabled.
    EXPECT_EQ(app::analysis_input_sha256(p), active);
    s.consider_analysis = false;
    const auto no_consider = app::analysis_input_sha256(p);
    const auto no_consider_report = runtime::report_input_sha256(p);
    s.srp.variance *= 4;
    s.drag_srp_covariance *= 2;
    s.drag_enabled = false;
    EXPECT_EQ(app::analysis_input_sha256(p), no_consider);
    EXPECT_EQ(runtime::report_input_sha256(p), no_consider_report);
    s.state[1][1] *= 2;
    EXPECT_NE(app::analysis_input_sha256(p), no_consider);
}
TEST(HpopCovarianceProject, RejectsInvalidJointMatricesDraftsAndUnsupportedEvents) {
    const auto valid = example();
    auto p = valid;
    auto &o = p.satellites[0].orbit;
    o.covariance.drag.state_cross_covariance[0] = 100;
    EXPECT_THROW((void)app::encode_project(p), app::ProjectError);
    o.covariance.enabled = false;
    EXPECT_NO_THROW((void)app::encode_project(p)); // Finite non-PD disabled draft.
    o.covariance.state[0][0] = std::numeric_limits<double>::quiet_NaN();
    EXPECT_THROW((void)app::encode_project(p), app::ProjectError);
    o = valid.satellites[0].orbit;
    o.covariance.gravity.degree = 1;
    EXPECT_THROW((void)app::encode_project(p), app::ProjectError);
    o = valid.satellites[0].orbit;
    o.hpop.srp.enabled = true;
    o.hpop.srp.shadow_model = core::SrpShadowModel::cylindrical;
    EXPECT_THROW((void)app::encode_project(p), app::ProjectError);
    o = valid.satellites[0].orbit;
    o.propagator = app::OrbitPropagator::mission;
    o.mission.coast_model = core::MissionCoastModel::hpop;
    o.mission.segments = {{core::MissionSegmentKind::coast, "Coast", 120000000000LL}};
    EXPECT_THROW((void)app::encode_project(p), app::ProjectError);
}
TEST(HpopCovarianceProject, PreparedQueriesUseCapturedConfigurationAndCancellation) {
    const auto p = example();
    app::ProjectDocument document{p}, other{p};
    const auto ref = document.objects().get("satellite-1");
    const auto analysis = runtime::prepare(document);
    for (auto at : {p.first, core::Epoch{0}, p.last}) {
        const auto result = analysis.covariance(ref, at);
        EXPECT_EQ(result.variational.state.epoch, at);
        EXPECT_EQ(result.covariance.drag->variance, p.satellites[0].orbit.covariance.drag.variance);
        EXPECT_EQ(result.covariance.drag_srp_covariance,
                  p.satellites[0].orbit.covariance.drag_srp_covariance);
        EXPECT_EQ(result.covariance.state[1][0], result.covariance.state[0][1]);
    }
    EXPECT_THROW((void)analysis.covariance(other.objects().get("satellite-1"), p.last),
                 app::ProjectError);
    EXPECT_THROW((void)analysis.covariance(ref, core::Epoch{60000000001}), std::out_of_range);
    int calls = 0;
    EXPECT_THROW((void)analysis.covariance(ref, p.last, [&] { return ++calls == 20; }),
                 runtime::AnalysisCancelled);
    EXPECT_EQ(calls, 20);
    EXPECT_NO_THROW((void)analysis.covariance(ref, p.last));
}
TEST(HpopCovarianceProject, ReportUsesSameNumericsAndSquaredUnitScaling) {
    const auto p = example();
    app::ProjectDocument document{p};
    const auto analysis = runtime::prepare(document);
    const auto ref = document.objects().get("satellite-1");
    const auto result = analysis.report(report());
    ASSERT_EQ(result.table.rows().size(), 3);
    ASSERT_EQ(result.table.columns().size(), 25);
    EXPECT_EQ(result.algorithm, "hpop-joint-covariance-direct-coast-v1");
    EXPECT_EQ(result.column_unit(4), "km^2");
    EXPECT_EQ(result.column_unit(10), "km^2/s");
    for (std::size_t row = 0; row < 3; ++row) {
        const auto epoch = std::get<core::Epoch>(result.table.rows()[row][2]);
        const auto exact = analysis.covariance(ref, epoch);
        std::size_t column = 4;
        for (std::size_t i = 0; i < 6; ++i)
            for (std::size_t j = 0; j <= i; ++j, ++column) {
                EXPECT_EQ(std::get<double>(result.table.rows()[row][column]),
                          exact.covariance.state[i][j]);
                EXPECT_EQ(*result.numeric_value(row, column), exact.covariance.state[i][j] / 1e6);
            }
    }
    const auto direct = runtime::generate_report(runtime::ReportContext{p, {}, {}, {}}, report());
    EXPECT_EQ(direct.table.rows(), result.table.rows());
    auto disabled = p;
    disabled.satellites[0].orbit.covariance.enabled = false;
    EXPECT_THROW((void)runtime::estimate_report_rows(disabled, report()), std::invalid_argument);
}
TEST(HpopCovarianceProject, CovarianceResourceRequirementsJoinNominalRequirements) {
    auto p = example();
    auto &o = p.satellites[0].orbit;
    o.initial.epoch = core::Epoch{-120000000000};
    o.covariance.coordinates.frame = core::HpopCovarianceFrame::fk5_j2000;
    EXPECT_TRUE(o.requires_eop());
    EXPECT_EQ(app::calculation_data_interval(p).first, o.initial.epoch);
    app::ProjectDocument missing{p};
    EXPECT_THROW((void)runtime::prepare(missing), std::invalid_argument);
    o.covariance.coordinates.frame = core::HpopCovarianceFrame::gcrf;
    o.covariance.independent_gravity = true;
    o.hpop.solid_tides.mode = core::SolidTideMode::full;
    o.hpop.solid_tides.truncate_to_gravity = true;
    EXPECT_FALSE(o.hpop.uses_ephemeris());
    EXPECT_TRUE(o.hpop_resource_options().uses_ephemeris());
    EXPECT_EQ(o.hpop.gravity_degree, 0);
    o.hpop.solid_tides.include_time_dependent_terms = true;
    const auto before = app::analysis_input_sha256(p);
    const auto before_report = runtime::report_input_sha256(p);
    o.hpop.solid_tides.include_time_dependent_terms = false;
    EXPECT_NE(app::analysis_input_sha256(p), before);
    EXPECT_NE(runtime::report_input_sha256(p), before_report);
    if (runtime::time_frames_available()) {
        o = example().satellites[0].orbit;
        o.covariance.coordinates.frame = core::HpopCovarianceFrame::true_of_date;
        const auto data = runtime::DataSnapshot::load(ORBITFORGE_REPORT_EOP);
        app::ProjectDocument d{p};
        const auto analysis = runtime::prepare(d, data);
        EXPECT_NO_THROW((void)analysis.covariance(d.objects().get("satellite-1"), p.last));
    }
}
