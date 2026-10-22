#include <algorithm>
#include <cmath>
#include <fstream>
#include <gtest/gtest.h>
#include <limits>
#include <orbelle/core/propagation/relative_motion.hpp>
#include <orbelle/runtime/analysis.hpp>
#include <orbelle/runtime/gravity.hpp>
#include <orbelle/runtime/project.hpp>
#include <orbelle/runtime/time_frames.hpp>
#include <orbelle/runtime/two_body.hpp>
#include <sstream>
using namespace orbelle;
namespace {
core::Epoch seconds(double v) {
    return core::Epoch{static_cast<std::int64_t>(std::llround(v * 1e9))};
}
class IdentityFrames final : public core::IFrameProvider {
    core::PositionRotation gcrf_to_itrf(core::Epoch epoch,
                                        const core::EarthOrientationSample &) const override {
        return {epoch,
                core::OriginId::earth,
                core::FrameId::gcrf,
                core::FrameId::itrf,
                {{{1, 0, 0}, {0, 1, 0}, {0, 0, 1}}},
                "Test inertial Earth",
                "test"};
    }
    core::StateTransform gcrf_to_itrf_state(core::Epoch epoch,
                                            const core::EarthOrientationState &eop) const override {
        return {gcrf_to_itrf(epoch, eop.sample), {}, "Test zero rotation"};
    }
};

application::ReportDefinition
definition(application::ReportKind kind = application::ReportKind::intersatellite_access) {
    application::ReportDefinition d;
    d.id = d.name = "intersatellite";
    d.query.kind = kind;
    d.query.object_ids = {"primary", "target"};
    d.query.step_nanoseconds = 60'000'000'000;
    d.query.event_tolerance_nanoseconds = 1000;
    d.format.time_scale = application::ReportTimeScale::tai;
    d.format.units = application::ReportUnits::si;
    return d;
}
runtime::ReportContext context(double first = 0, double last = 600) {
    application::Project p{"Intersatellite", seconds(first),
                           seconds(last > first ? last : first + 1)};
    for (const auto &id : {"primary", "target"}) {
        application::ProjectSatellite s;
        s.orbit.object_id = id;
        s.display_name = id;
        s.orbit.first = p.first;
        s.orbit.last = p.last;
        s.orbit.initial = {
            seconds(0), core::OriginId::earth, core::FrameId::gcrf, {7e6, 0, 0}, {0, 7500, 100}};
        p.satellites.push_back(s);
    }
    auto eop = runtime::load_eop_snapshot(ORBELLE_REPORT_EOP);
    p.earth_orientation = eop->data_info();
    return {p, runtime::make_builtin_time_provider(), std::make_shared<IdentityFrames>(),
            std::move(eop)};
}
struct Reference {
    std::string name;
    double first, last;
    bool flat;
    runtime::ReportContext context;
    std::vector<std::array<double, 8>> samples;
    std::vector<std::array<double, 6>> windows;
    std::array<double, 4> extrema{};
};
std::vector<Reference> references() {
    std::ifstream file(ORBELLE_INTERSATELLITE_REFERENCE);
    if (!file)
        throw std::runtime_error("Missing intersatellite reference");
    std::vector<Reference> cases;
    std::string line;
    while (std::getline(file, line)) {
        std::istringstream in(line);
        char type = 0;
        in >> type;
        if (type == 'C') {
            std::string name;
            double first, last;
            bool flat;
            in >> name >> first >> last >> flat;
            auto c = context(first, last);
            for (auto &s : c.project.satellites) {
                for (auto &v : s.orbit.initial.position_m)
                    in >> v;
                for (auto &v : s.orbit.initial.velocity_mps)
                    in >> v;
            }
            cases.push_back({name, first, last, flat, std::move(c), {}, {}, {}});
        } else if (type == 'S') {
            std::array<double, 8> a{};
            for (auto &v : a)
                in >> v;
            cases.back().samples.push_back(a);
        } else if (type == 'W') {
            std::array<double, 6> a{};
            for (auto &v : a)
                in >> v;
            cases.back().windows.push_back(a);
        } else if (type == 'G')
            for (auto &v : cases.back().extrema)
                in >> v;
    }
    return cases;
}
} // namespace
TEST(Intersatellite, MatchesOrekitQswDirectViewAndHipparchusExtrema) {
    if (!runtime::two_body_available() || !runtime::time_frames_available())
        GTEST_SKIP();
    const auto cases = references();
    ASSERT_EQ(cases.size(), 9U);
    double max_position = 0, max_velocity = 0, max_boundary = 0, max_extremum_time = 0;
    for (const auto &c : cases) {
        SCOPED_TRACE(c.name);
        auto d = definition(application::ReportKind::relative_motion);
        d.query.follow_scenario = false;
        d.query.first = seconds(c.first);
        d.query.last = seconds(c.last);
        d.query.step_nanoseconds = 137'000'000'000;
        const auto sampled = runtime::generate_report(c.context, d);
        ASSERT_EQ(sampled.table.rows().size(), c.samples.size());
        for (std::size_t i = 0; i < c.samples.size(); ++i) {
            const auto &row = sampled.table.rows()[i];
            const auto &ref = c.samples[i];
            EXPECT_EQ(std::get<core::Epoch>(row[2]), seconds(ref[0]));
            for (std::size_t j = 1; j < 7; ++j) {
                const auto error = std::abs(std::get<double>(row[j + 2]) - ref[j]);
                const bool velocity = j == 2 || j == 3;
                if (velocity)
                    max_velocity = std::max(max_velocity, error);
                else
                    max_position = std::max(max_position, error);
                EXPECT_LE(error, velocity ? 3e-7 : 2e-4);
            }
            EXPECT_EQ(std::get<std::int64_t>(row[9]), static_cast<std::int64_t>(ref[7]));
        }
        for (auto mode :
             {core::AccessSearchMode::fixed_grid, core::AccessSearchMode::extremum_assisted})
            for (auto step : {60'000'000'000LL, 30'000'000'000LL}) {
                d.query.kind = application::ReportKind::intersatellite_access;
                d.query.event_search_mode = mode;
                d.query.step_nanoseconds = step;
                const auto result = runtime::generate_report(c.context, d);
                ASSERT_EQ(result.table.rows().size(), c.windows.size());
                ASSERT_TRUE(result.intersatellite_statistics);
                const auto &stats = *result.intersatellite_statistics;
                EXPECT_EQ(stats.visibility.count, c.windows.size());
                double duration = 0;
                for (std::size_t i = 0; i < c.windows.size(); ++i) {
                    const auto &w = c.windows[i];
                    const auto &row = result.table.rows()[i];
                    for (std::size_t j = 0; j < 2; ++j) {
                        const auto error = std::abs(
                            std::get<core::Epoch>(row[j + 2]).seconds_since(seconds(0)) - w[j]);
                        max_boundary = std::max(max_boundary, error);
                        EXPECT_LE(error, 1.1e-6);
                    }
                    EXPECT_EQ(std::get<std::int64_t>(row[5]), static_cast<std::int64_t>(w[2]));
                    EXPECT_EQ(std::get<std::int64_t>(row[6]), static_cast<std::int64_t>(w[3]));
                    // A minimum at a visibility edge inherits up to 1 us of event-time error.
                    const bool at_edge =
                        std::min(std::abs(w[4] - w[0]), std::abs(w[4] - w[1])) < .003;
                    EXPECT_NEAR(std::get<double>(row[7]), w[5], at_edge ? .02 : 2e-4);
                    if (!c.flat) {
                        const auto error = std::abs(
                            std::get<core::Epoch>(row[8]).seconds_since(seconds(0)) - w[4]);
                        max_extremum_time = std::max(max_extremum_time, error);
                        EXPECT_LE(error, .003);
                    }
                    duration += w[1] - w[0];
                }
                EXPECT_NEAR(stats.visibility.total_duration_s, duration,
                            1.1e-6 * 2 * static_cast<double>(c.windows.size()));
                for (std::size_t j = 0; j < 2; ++j) {
                    const auto &v = j ? stats.maximum_range : stats.minimum_range;
                    EXPECT_NEAR(v.value, c.extrema[j * 2 + 1], 2e-4);
                    if (!c.flat) {
                        const auto error =
                            std::abs(v.epoch.seconds_since(seconds(0)) - c.extrema[j * 2]);
                        max_extremum_time = std::max(max_extremum_time, error);
                        EXPECT_LE(error, .003);
                    }
                }
            }
    }
    std::ostringstream m;
    m.precision(17);
    m << "position_m=" << max_position << "; velocity_mps=" << max_velocity
      << "; boundary_s=" << max_boundary << "; extremum_time_s=" << max_extremum_time;
    RecordProperty("maximum_reference_errors", m.str());
}
TEST(Intersatellite, AxesCoincidenceSymmetryTangencyAndInvalidStates) {
    core::CartesianState a{
        seconds(0), core::OriginId::earth, core::FrameId::gcrf, {7e6, 0, 0}, {0, 7500, 0}};
    auto b = a;
    b.position_m = {7e6 + 3, 4, 12};
    b.velocity_mps = {1, 7502, 2};
    const auto m = core::relative_motion(a, b);
    EXPECT_DOUBLE_EQ(m.range_m, 13);
    EXPECT_DOUBLE_EQ(m.relative_speed_mps, 3);
    EXPECT_EQ(m.ric_position_m, (std::array<double, 3>{3, 4, 12}));
    EXPECT_NEAR(*m.range_rate_mps, 35.0 / 13, 1e-14);
    EXPECT_NEAR(*core::relative_motion(b, a).range_rate_mps, *m.range_rate_mps, 1e-14);
    EXPECT_FALSE(core::relative_motion(a, a).range_rate_mps);
    EXPECT_EQ(core::relative_motion(a, a).ric_position_m, (std::array<double, 3>{}));
    a.frame = b.frame = core::FrameId::itrf;
    a.position_m = {6378137, -1e6, 0};
    b.position_m = {6378137, 1e6, 0};
    EXPECT_DOUBLE_EQ(core::intersatellite_clearance(a, b), 0);
    b.position_m[0] -= 1;
    EXPECT_LT(core::intersatellite_clearance(a, b), 0);
    EXPECT_DOUBLE_EQ(core::intersatellite_clearance(a, b), core::intersatellite_clearance(b, a));
    a.position_m = {0, 0, 6356752.314245179 + 100};
    b = a;
    EXPECT_GT(core::intersatellite_clearance(a, b), 0);
    for (int fault = 0; fault < 5; ++fault) {
        auto bad = b;
        if (fault == 0)
            bad.epoch = seconds(1);
        if (fault == 1)
            bad.frame = core::FrameId::gcrf;
        if (fault == 2)
            bad.position_m[0] = std::numeric_limits<double>::quiet_NaN();
        if (fault == 3)
            bad.position_m = {0, 0, 0};
        if (fault == 4)
            bad.velocity_mps[0] = std::numeric_limits<double>::infinity();
        EXPECT_THROW((void)core::intersatellite_clearance(a, bad), std::invalid_argument);
    }
    a.frame = b.frame = core::FrameId::gcrf;
    a.velocity_mps = {0, 0, 1};
    EXPECT_THROW((void)core::relative_motion(a, b), std::invalid_argument);
    a.velocity_mps = {};
    EXPECT_THROW((void)core::relative_motion(a, b), std::invalid_argument);
}
TEST(Intersatellite, OrderedPairValidationCancellationAndTemplates) {
    if (!runtime::two_body_available() || !runtime::time_frames_available())
        GTEST_SKIP();
    auto c = references()[1].context;
    c.project.last = seconds(600);
    for (auto &sat : c.project.satellites)
        sat.orbit.last = c.project.last;
    auto d = definition(application::ReportKind::relative_motion);
    EXPECT_EQ(runtime::estimate_report_rows(c.project, d), 11U);
    std::size_t completed = 0;
    auto result = runtime::generate_report(c, d, nullptr, [&](auto count, auto total) {
        EXPECT_GE(count, completed);
        EXPECT_LE(count, total);
        completed = count;
    });
    EXPECT_EQ(completed, 11U);
    auto format = d.format;
    format.columns = {{"relative.range", "km", "Distance"}};
    const auto formatted = result.with_template(format);
    ASSERT_TRUE(formatted.intersatellite_statistics);
    EXPECT_EQ(formatted.table.columns().size(), 1U);
    EXPECT_NEAR(*formatted.numeric_value(0, 0), *result.numeric_value(0, 3) / 1000, 1e-12);
    EXPECT_EQ(formatted.with_template(d.format).table.rows().size(), result.table.rows().size());
    std::swap(d.query.object_ids[0], d.query.object_ids[1]);
    auto reverse = runtime::generate_report(c, d);
    EXPECT_NEAR(*reverse.numeric_value(0, 3), *result.numeric_value(0, 3), 1e-8);
    EXPECT_NEAR(*reverse.numeric_value(0, 4), *result.numeric_value(0, 4), 1e-8);
    EXPECT_NE(*reverse.numeric_value(0, 6), *result.numeric_value(0, 6));
    std::atomic_bool cancel = true;
    EXPECT_THROW((void)runtime::generate_report(c, d, &cancel), runtime::ReportCancelled);
    cancel = false;
    EXPECT_THROW((void)runtime::generate_report(c, d, &cancel, [&](auto, auto) { cancel = true; }),
                 runtime::ReportCancelled);
    c.frames.reset();
    EXPECT_THROW((void)runtime::generate_report(c, d), std::invalid_argument);
    d.query.object_ids = {"primary", "primary"};
    EXPECT_THROW(application::validate_report_definition(d), std::invalid_argument);
    d.query.object_ids = {"primary"};
    EXPECT_THROW(application::validate_report_definition(d), std::invalid_argument);
    d.query.object_ids = {"primary", "target"};
    d.query.scope = application::ReportScope::all_compatible;
    EXPECT_THROW(application::validate_report_definition(d), std::invalid_argument);
}

TEST(Intersatellite, HpopScientificEphemerisMatchesIndependentOrekit) {
    if (!runtime::time_frames_available())
        GTEST_SKIP();
    const auto ref = references()[0];
    auto c = ref.context;
    core::GravityInfo info{"Relative point mass",
                           std::string(64, 'a'),
                           "",
                           "tide_free",
                           3.986004418e14,
                           6378137.,
                           2,
                           0};
    auto gravity =
        std::make_shared<core::GravityModel>(info, std::vector<core::GravityCoefficient>{});
    c.gravity.emplace("relative-reference", gravity);
    c.project.force_data.push_back(runtime::gravity_reference("relative-reference", info));
    for (auto &s : c.project.satellites) {
        s.orbit.propagator = application::OrbitPropagator::hpop;
        auto &o = s.orbit.hpop;
        o.gravity_degree = o.gravity_order = 0;
        o.gravity_mu = 3.986004418e14;
        o.gravity_model = "relative-reference";
        o.integrator = core::HpopIntegrator::rkv89;
        o.relative_tolerance = 1e-14;
        o.position_tolerance_m = 1e-8;
        o.velocity_tolerance_mps = 1e-11;
        o.max_step_seconds = 30;
        o.report_ephemeris_on_fixed_step = true;
        s.orbit.step_seconds = 15;
    }
    auto d = definition(application::ReportKind::relative_motion);
    d.query.step_nanoseconds = 137'000'000'000;
    const auto result = runtime::generate_report(c, d);
    ASSERT_EQ(result.table.rows().size(), ref.samples.size());
    for (std::size_t i = 0; i < ref.samples.size(); ++i) {
        const auto &row = result.table.rows()[i];
        const auto &sample = ref.samples[i];
        for (std::size_t j = 1; j < 7; ++j)
            EXPECT_NEAR(std::get<double>(row[j + 2]), sample[j], j == 2 || j == 3 ? 2e-5 : .02);
        EXPECT_EQ(std::get<std::int64_t>(row[9]), static_cast<std::int64_t>(sample[7]));
    }
    const auto &windows = result.intersatellite_statistics->windows;
    ASSERT_EQ(windows.size(), ref.windows.size());
    for (std::size_t i = 0; i < windows.size(); ++i) {
        EXPECT_NEAR(windows[i].first.seconds_since(seconds(0)), ref.windows[i][0], 2e-6);
        EXPECT_NEAR(windows[i].last.seconds_since(seconds(0)), ref.windows[i][1], 2e-6);
    }
}
TEST(Intersatellite, SnapshotUsesConfirmedFramesAndPreservesOldResults) {
    if (!runtime::time_frames_available() || !runtime::two_body_available())
        GTEST_SKIP();
    auto c = context();
    c.frames = runtime::make_frame_provider();
    c.project.satellites[1].orbit.initial.position_m[0] += 100000;
    application::ProjectDocument document(c.project);
    auto analysis =
        runtime::prepare(document, runtime::DataSnapshot::from_providers(c.time, c.frames, c.eop));
    auto d = definition(application::ReportKind::relative_motion);
    const auto result = analysis.report(d);
    const auto expected = runtime::generate_report(c, d);
    EXPECT_EQ(result.table.rows(), expected.table.rows());
    EXPECT_EQ(result.input_revision, document.snapshot().revision);
    EXPECT_EQ(result.data_sha256, expected.data_sha256);
    d.query.object_ids = {"missing", "target"};
    EXPECT_THROW((void)analysis.report(d), std::invalid_argument);
    d = definition();
    EXPECT_THROW((void)analysis.report(d, [] { return true; }), runtime::AnalysisCancelled);
    auto mission = c.project.satellites[0].orbit;
    mission.propagator = application::OrbitPropagator::mission;
    mission.mission.segments = {core::MissionSegment::coast(std::chrono::seconds{600})};
    c.project.satellites[0].orbit = mission;
    EXPECT_THROW((void)runtime::generate_report(c, d), std::invalid_argument);
}
