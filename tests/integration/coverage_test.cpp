#include <algorithm>
#include <cmath>
#include <fstream>
#include <gtest/gtest.h>
#include <limits>
#include <numbers>
#include <numeric>
#include <orbelle/runtime/gravity.hpp>
#include <orbelle/runtime/project.hpp>
#include <orbelle/runtime/report.hpp>
#include <orbelle/runtime/time_frames.hpp>
#include <orbelle/runtime/two_body.hpp>
#include <sstream>
using namespace orbelle;
namespace {
constexpr double pi = std::numbers::pi;
core::Epoch seconds(double t) {
    return core::Epoch{static_cast<std::int64_t>(std::llround(t * 1e9))};
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

struct Point {
    double lon = 0, lat = 0, area = 0, covered = 0, max = 0, mean = 0, internal_max = 0,
           internal_mean = 0;
    std::vector<std::pair<double, double>> windows;
};
struct Case {
    std::string name;
    core::CoverageGrid grid;
    int mode = 0;
    std::vector<Point> points;
};
std::vector<Case> reference() {
    std::ifstream file(ORBELLE_COVERAGE_REFERENCE);
    if (!file)
        throw std::runtime_error("Missing coverage reference");
    std::vector<Case> cases;
    for (std::string line; std::getline(file, line);) {
        std::istringstream in(line);
        char type;
        in >> type;
        if (type == 'C') {
            Case c;
            in >> c.name >> c.grid.south_rad >> c.grid.north_rad >> c.grid.west_rad >>
                c.grid.east_rad >> c.grid.resolution_rad >> c.grid.minimum_elevation_rad >> c.mode;
            cases.push_back(c);
        } else if (type == 'P') {
            int i;
            Point p;
            in >> i >> p.lon >> p.lat >> p.area;
            cases.back().points.push_back(p);
        } else if (type == 'W') {
            double a, b;
            in >> a >> b;
            cases.back().points.back().windows.emplace_back(a, b);
        } else if (type == 'S') {
            auto &p = cases.back().points.back();
            in >> p.covered >> p.max >> p.mean >> p.internal_max >> p.internal_mean;
        } else
            throw std::runtime_error("Unknown coverage reference record");
        if (!in)
            throw std::runtime_error("Incomplete coverage reference");
    }
    return cases;
}
runtime::ReportContext context(int mode = 0) {
    application::Project p{"Coverage", seconds(0), seconds(12000)};
    for (int i = 0; i < 2; ++i) {
        application::ProjectSatellite s;
        const double c = std::cos(double(i)), t = std::sin(double(i));
        s.orbit.object_id = "sat" + std::to_string(i);
        s.orbit.first = p.first;
        s.orbit.last = p.last;
        s.orbit.initial = {p.first,
                           core::OriginId::earth,
                           core::FrameId::gcrf,
                           {7e6 * c, 7e6 * t, 0},
                           {-6200 * t, 6200 * c, 5300}};
        p.satellites.push_back(s);
        if (mode)
            p.sensors.push_back({"sensor" + std::to_string(i), "Sensor", s.orbit.object_id, .3,
                                 mode == 1 ? application::SensorPointing::nadir
                                           : application::SensorPointing::targeted,
                                 mode == 1 ? "" : "anchor"});
    }
    p.ground_stations.push_back({"anchor", "Anchor", {.2, .1, 0}});
    auto eop = runtime::load_eop_snapshot(ORBELLE_REPORT_EOP);
    p.earth_orientation = eop->data_info();
    return {p, runtime::make_builtin_time_provider(), std::make_shared<IdentityFrames>(),
            std::move(eop)};
}
application::ReportDefinition definition(int mode = 0) {
    application::ReportDefinition d;
    d.id = d.name = "Coverage";
    d.query.kind = application::ReportKind::coverage_grid;
    d.query.object_ids = mode ? std::vector<std::string>{"sensor0", "sensor1"}
                              : std::vector<std::string>{"sat0", "sat1"};
    d.query.step_nanoseconds = 60'000'000'000;
    d.query.event_tolerance_nanoseconds = 1000;
    return d;
}
} // namespace
TEST(Coverage, OrekitMultiAssetWindowsAndHipparchusEllipsoidAreas) {
    if (!runtime::time_frames_available() || !runtime::two_body_available())
        GTEST_SKIP();
    for (const auto &c : reference()) {
        SCOPED_TRACE(c.name);
        auto ctx = context(c.mode);
        auto d = definition(c.mode);
        d.query.coverage = c.grid;
        for (const bool ends : {true, false}) {
            d.query.coverage.include_end_gaps = ends;
            const auto r = runtime::generate_report(ctx, d);
            ASSERT_TRUE(r.coverage_statistics);
            const auto &s = *r.coverage_statistics;
            ASSERT_EQ(s.points.size(), c.points.size());
            ASSERT_EQ(r.table.rows().size(), c.points.size());
            double area = 0, ever = 0, weighted = 0, maxgap = 0;
            for (std::size_t i = 0; i < c.points.size(); ++i) {
                SCOPED_TRACE(i);
                const auto &ref = c.points[i];
                const auto &point = s.points[i];
                EXPECT_NEAR(point.cell.location.longitude_rad, ref.lon, 1e-14);
                EXPECT_NEAR(point.cell.location.latitude_rad, ref.lat, 1e-14);
                EXPECT_NEAR(point.cell.area_m2, ref.area, ref.area * 3e-14);
                ASSERT_EQ(point.windows.size(), ref.windows.size());
                for (std::size_t j = 0; j < ref.windows.size(); ++j) {
                    EXPECT_NEAR(point.windows[j].first.seconds_since(seconds(0)),
                                ref.windows[j].first, 2e-6);
                    EXPECT_NEAR(point.windows[j].last.seconds_since(seconds(0)),
                                ref.windows[j].second, 2e-6);
                }
                EXPECT_NEAR(point.statistics.covered_seconds, ref.covered, 8e-6);
                EXPECT_NEAR(point.statistics.maximum_gap_seconds, ends ? ref.max : ref.internal_max,
                            3e-6);
                EXPECT_NEAR(point.statistics.mean_gap_seconds, ends ? ref.mean : ref.internal_mean,
                            3e-6);
                area += ref.area;
                if (!ref.windows.empty())
                    ever += ref.area;
                weighted += ref.area * ref.covered / 12000;
                maxgap = std::max(maxgap, ends ? ref.max : ref.internal_max);
            }
            EXPECT_NEAR(s.total_area_m2, area, area * 3e-14);
            EXPECT_NEAR(s.ever_covered_area_m2, ever, area * 3e-14);
            EXPECT_NEAR(s.mean_coverage_fraction, weighted / area, 8e-10);
            EXPECT_NEAR(s.maximum_gap_seconds, maxgap, 3e-6);
        }
        // Independent timestamp membership from Orekit windows also verifies cumulative area.
        d.query.kind = application::ReportKind::coverage_time;
        d.query.step_nanoseconds = 30'000'000'000;
        const auto series = runtime::generate_report(ctx, d);
        ASSERT_EQ(series.table.rows().size(), 401U);
        for (std::size_t i = 0; i < series.table.rows().size(); ++i) {
            double instant = 0, ever = 0, total = 0, t = i * 30.;
            for (const auto &p : c.points) {
                total += p.area;
                if (std::ranges::any_of(p.windows,
                                        [&](auto w) { return w.first <= t && t <= w.second; }))
                    instant += p.area;
                if (!p.windows.empty() && p.windows.front().first <= t)
                    ever += p.area;
            }
            EXPECT_NEAR(std::get<double>(series.table.rows()[i][3]), instant, total * 3e-14);
            EXPECT_NEAR(std::get<double>(series.table.rows()[i][4]), instant / total, 3e-14);
            EXPECT_NEAR(std::get<double>(series.table.rows()[i][5]), ever, total * 3e-14);
            EXPECT_NEAR(std::get<double>(series.table.rows()[i][6]), ever / total, 3e-14);
        }
    }
}
TEST(Coverage, UnionRevisitEndGapsNeverAndAlwaysCovered) {
    const auto a = seconds(0), b = seconds(100);
    const std::vector<core::AccessInterval> input{{seconds(50), seconds(70)},
                                                  {seconds(10), seconds(30)},
                                                  {seconds(20), seconds(40)},
                                                  {seconds(40), seconds(50)},
                                                  {seconds(10), seconds(30)}};
    const auto merged = core::merge_coverage_intervals(a, b, input);
    ASSERT_EQ(merged.size(), 1U);
    EXPECT_EQ(merged[0].first, seconds(10));
    EXPECT_EQ(merged[0].last, seconds(70));
    const auto stats = core::coverage_time_statistics(a, b, input, true);
    EXPECT_DOUBLE_EQ(stats.covered_seconds, 60);
    EXPECT_DOUBLE_EQ(stats.maximum_gap_seconds, 30);
    EXPECT_DOUBLE_EQ(stats.mean_gap_seconds, 20);
    EXPECT_EQ(stats.gap_count, 2U);
    EXPECT_DOUBLE_EQ(core::coverage_time_statistics(a, b, input, false).maximum_gap_seconds, 0);
    for (bool ends : {true, false}) {
        const auto empty = core::coverage_time_statistics(a, b, {}, ends);
        EXPECT_DOUBLE_EQ(empty.maximum_gap_seconds, 100);
        EXPECT_DOUBLE_EQ(empty.mean_gap_seconds, 100);
        EXPECT_EQ(empty.gap_count, 1U);
        const auto full = core::coverage_time_statistics(a, b, {{a, b}}, ends);
        EXPECT_DOUBLE_EQ(full.maximum_gap_seconds, 0);
        EXPECT_EQ(full.gap_count, 0U);
    }
    EXPECT_THROW((void)core::merge_coverage_intervals(a, b, {{seconds(-1), seconds(1)}}),
                 std::invalid_argument);
    EXPECT_THROW((void)core::merge_coverage_intervals(a, b, {{seconds(2), seconds(1)}}),
                 std::invalid_argument);
    EXPECT_THROW((void)core::coverage_time_statistics(a, a, {}, true), std::invalid_argument);
}
TEST(Coverage, LimitsValidationCancellationAndFormatting) {
    core::CoverageGrid g;
    g.south_rad = -pi / 2;
    g.north_rad = pi / 2;
    g.west_rad = -pi;
    g.east_rad = pi;
    g.resolution_rad = 1e-12;
    EXPECT_THROW((void)core::coverage_grid(g), std::invalid_argument);
    g.resolution_rad = .1;
    g.east_rad = g.west_rad;
    EXPECT_THROW((void)core::coverage_grid(g), std::invalid_argument);
    if (!runtime::time_frames_available() || !runtime::two_body_available())
        GTEST_SKIP();
    auto ctx = context();
    auto d = definition();
    d.query.coverage = {-.05, .05, -.05, .05, .2, 0, true};
    const auto result = runtime::generate_report(ctx, d);
    ASSERT_TRUE(result.coverage_statistics);
    auto format = d.format;
    format.columns = {{"coverage.area", "km^2", "Area"}, {"coverage.fraction", "%", "Coverage"}};
    const auto formatted = result.with_template(format);
    EXPECT_EQ(formatted.table.rows().size(), 1U);
    EXPECT_NEAR(*formatted.numeric_value(0, 0),
                result.coverage_statistics->points[0].cell.area_m2 / 1e6, 1e-6);
    EXPECT_EQ(formatted.coverage_statistics->points[0].windows.size(),
              result.coverage_statistics->points[0].windows.size());
    const std::atomic_bool cancelled{true};
    EXPECT_THROW((void)runtime::generate_report(ctx, d, &cancelled), runtime::ReportCancelled);
    auto bad = d;
    bad.query.object_ids = {"missing"};
    EXPECT_THROW((void)runtime::estimate_report_rows(ctx.project, bad), std::invalid_argument);
    bad = d;
    bad.query.object_ids.clear();
    bad.query.scope = application::ReportScope::all_compatible;
    EXPECT_THROW((void)runtime::estimate_report_rows(ctx.project, bad), std::invalid_argument);
    bad = d;
    bad.query.follow_scenario = false;
    bad.query.first = bad.query.last = seconds(0);
    EXPECT_THROW((void)runtime::estimate_report_rows(ctx.project, bad), std::invalid_argument);
    bad = d;
    bad.query.follow_scenario = false;
    bad.query.first = seconds(0);
    bad.query.last = core::Epoch{199999500000};
    bad.query.step_nanoseconds = 1000000;
    EXPECT_THROW((void)runtime::estimate_report_rows(ctx.project, bad), std::invalid_argument);
    bad = d;
    bad.query.coverage.minimum_elevation_rad = std::numeric_limits<double>::quiet_NaN();
    EXPECT_THROW((void)runtime::estimate_report_rows(ctx.project, bad), std::invalid_argument);
    ctx.eop.reset();
    EXPECT_THROW((void)runtime::generate_report(ctx, d), std::invalid_argument);
}

TEST(Coverage, HpopScientificEphemerisMatchesOrekitWindows) {
    if (!runtime::time_frames_available())
        GTEST_SKIP();
    const auto ref = reference().front();
    auto c = context();
    core::GravityInfo info{"Coverage point mass",
                           std::string(64, 'a'),
                           "",
                           "tide_free",
                           3.986004418e14,
                           6378137.,
                           2,
                           0};
    auto gravity =
        std::make_shared<core::GravityModel>(info, std::vector<core::GravityCoefficient>{});
    c.gravity.emplace("coverage-reference", gravity);
    c.project.force_data.push_back(runtime::gravity_reference("coverage-reference", info));
    for (auto &sat : c.project.satellites) {
        sat.orbit.propagator = application::OrbitPropagator::hpop;
        auto &o = sat.orbit.hpop;
        o.gravity_degree = o.gravity_order = 0;
        o.gravity_mu = 3.986004418e14;
        o.gravity_model = "coverage-reference";
        o.integrator = core::HpopIntegrator::rkv89;
        o.relative_tolerance = 1e-14;
        o.position_tolerance_m = 1e-8;
        o.velocity_tolerance_mps = 1e-11;
        o.max_step_seconds = 30;
        o.report_ephemeris_on_fixed_step = true;
        sat.orbit.step_seconds = 15;
    }
    auto d = definition();
    d.query.coverage = ref.grid;
    const auto result = runtime::generate_report(c, d);
    ASSERT_TRUE(result.coverage_statistics);
    ASSERT_EQ(result.coverage_statistics->points.size(), ref.points.size());
    for (std::size_t i = 0; i < ref.points.size(); ++i) {
        SCOPED_TRACE(i);
        const auto &point = result.coverage_statistics->points[i];
        ASSERT_EQ(point.windows.size(), ref.points[i].windows.size());
        for (std::size_t j = 0; j < point.windows.size(); ++j) {
            EXPECT_NEAR(point.windows[j].first.seconds_since(seconds(0)),
                        ref.points[i].windows[j].first, 2e-6);
            EXPECT_NEAR(point.windows[j].last.seconds_since(seconds(0)),
                        ref.points[i].windows[j].second, 2e-6);
        }
    }
}
