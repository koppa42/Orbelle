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
  public:
    mutable std::size_t state_calls = 0;

  private:
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
        ++state_calls;
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
TEST(Coverage, ThreeAxisSensorOffsetAffectsCoverageAndCapturedResultsStayUnchanged) {
    if (!runtime::time_frames_available() || !runtime::two_body_available()) {
        GTEST_SKIP();
    }
    auto ctx = context(1);
    auto d = definition(1);
    d.query.object_ids = {"sensor0"};
    d.query.follow_scenario = false;
    d.query.first = seconds(0);
    d.query.last = seconds(10);
    d.query.coverage = {-.01, .01, -.01, .01, .1, 0, true};
    const auto baseline = runtime::generate_report(ctx, d);
    ASSERT_TRUE(baseline.coverage_statistics);
    EXPECT_NEAR(baseline.coverage_statistics->mean_coverage_fraction, 1, 1e-12);
    // Either local tilt can turn the entire narrow cone away from Earth.
    for (bool use_x : {true, false}) {
        auto &sensor = ctx.project.sensors.front();
        sensor.tilt_x_rad = use_x ? pi : 0;
        sensor.tilt_y_rad = use_x ? 0 : pi;
        sensor.roll_rad = .7;
        const auto away = runtime::generate_report(ctx, d);
        ASSERT_TRUE(away.coverage_statistics);
        EXPECT_EQ(away.coverage_statistics->ever_covered_area_m2, 0);
        EXPECT_EQ(away.coverage_statistics->mean_coverage_fraction, 0);
        EXPECT_EQ(away.coverage_statistics->maximum_gap_seconds, 10);
    }
    EXPECT_NEAR(baseline.coverage_statistics->mean_coverage_fraction, 1, 1e-12);
}
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
    EXPECT_NO_THROW((void)runtime::estimate_report_rows(ctx.project, bad));
    bad.query.kind = application::ReportKind::coverage_time;
    EXPECT_GT(runtime::estimate_report_rows(ctx.project, bad), 200000);
    EXPECT_THROW((void)runtime::estimate_report_rows(ctx.project, bad, 64 * 1024 * 1024),
                 std::invalid_argument);
    bad = d;
    bad.query.coverage.minimum_elevation_rad = std::numeric_limits<double>::quiet_NaN();
    EXPECT_THROW((void)runtime::estimate_report_rows(ctx.project, bad), std::invalid_argument);
    ctx.eop.reset();
    EXPECT_THROW((void)runtime::generate_report(ctx, d), std::invalid_argument);
}

TEST(Coverage, ThousandAssetUnionRetainsValidationAndResourceBudgets) {
    if (!runtime::time_frames_available() || !runtime::two_body_available())
        GTEST_SKIP();
    auto ctx = context();
    auto d = definition();
    d.query.object_ids.clear();
    d.query.follow_scenario = false;
    d.query.first = seconds(0);
    d.query.last = seconds(10);
    d.query.coverage = {-.01, .01, -.01, .01, .1, 0, true};
    const auto seed = ctx.project.satellites.front();
    ctx.project.satellites.clear();
    for (std::size_t i = 0; i < 2048; ++i) {
        auto sat = seed;
        sat.orbit.object_id = "asset-" + std::to_string(i);
        d.query.object_ids.push_back(sat.orbit.object_id);
        ctx.project.satellites.push_back(std::move(sat));
    }
    const auto result = runtime::generate_report(ctx, d);
    ASSERT_TRUE(result.coverage_statistics);
    const auto &s = *result.coverage_statistics;
    ASSERT_EQ(s.points.size(), 1);
    ASSERT_EQ(s.points[0].windows.size(), 1);
    EXPECT_DOUBLE_EQ(s.mean_coverage_fraction, 1);
    EXPECT_DOUBLE_EQ(s.points[0].statistics.covered_seconds, 10);
    EXPECT_DOUBLE_EQ(s.maximum_gap_seconds, 0);
    auto bad = d;
    bad.query.object_ids.push_back("over-limit");
    EXPECT_THROW(application::validate_report_definition(bad), std::invalid_argument);
    bad = d;
    bad.query.object_ids.back() = "missing";
    EXPECT_THROW((void)runtime::generate_report(ctx, bad), std::invalid_argument);
    bad = d;
    bad.query.coverage.resolution_rad = .0001;
    bad.query.last = seconds(600);
    // Long-running work is permitted; only its explicitly budgeted storage is bounded.
    EXPECT_NO_THROW((void)runtime::estimate_report_rows(ctx.project, bad));
    EXPECT_THROW((void)runtime::estimate_report_rows(ctx.project, bad, 64 * 1024 * 1024),
                 std::invalid_argument);
    auto &last = ctx.project.satellites.back().orbit;
    last.first = last.initial.epoch = seconds(5);
    EXPECT_THROW((void)runtime::generate_report(ctx, d), application::ProjectError);
}

TEST(Coverage, ExactEpochFrameTransformsAreSharedAcrossAssets) {
    if (!runtime::time_frames_available() || !runtime::two_body_available())
        GTEST_SKIP();
    auto ctx = context();
    auto frames = std::make_shared<IdentityFrames>();
    ctx.frames = frames;
    auto d = definition();
    d.query.object_ids = {"sat0"};
    d.query.follow_scenario = false;
    d.query.first = seconds(0);
    d.query.last = seconds(600);
    d.query.event_search_mode = core::AccessSearchMode::fixed_grid;
    d.query.coverage = {-.01, .01, 3.0, 3.1, .2, 0, true};
    const auto single = runtime::generate_report(ctx, d);
    const auto calls = frames->state_calls;
    EXPECT_GT(calls, 2);
    frames->state_calls = 0;
    for (int i = 2; i < 65; ++i) {
        auto sat = ctx.project.satellites.front();
        sat.orbit.object_id = "sat" + std::to_string(i);
        d.query.object_ids.push_back(sat.orbit.object_id);
        ctx.project.satellites.push_back(std::move(sat));
    }
    const auto joint = runtime::generate_report(ctx, d);
    ASSERT_TRUE(joint.coverage_statistics);
    EXPECT_EQ(frames->state_calls, calls);
    EXPECT_DOUBLE_EQ(joint.coverage_statistics->mean_coverage_fraction,
                     single.coverage_statistics->mean_coverage_fraction);
    EXPECT_TRUE(joint.coverage_statistics->points[0].windows.empty());
    // Cancellation after work starts must stop a large selection as well.
    std::atomic_bool cancelled{false};
    EXPECT_THROW((void)runtime::generate_report(ctx, d, &cancelled,
                                                [&](std::size_t done, std::size_t) {
                                                    if (done)
                                                        cancelled = true;
                                                }),
                 runtime::ReportCancelled);
}

TEST(Coverage, LargeSelectionMatchesUnionOfIndependentFullIntervalSearches) {
    if (!runtime::time_frames_available() || !runtime::two_body_available())
        GTEST_SKIP();
    auto ctx = context();
    const auto seed = ctx.project.satellites.front();
    ctx.project.satellites.clear();
    auto d = definition();
    d.query.object_ids.clear();
    d.query.follow_scenario = false;
    d.query.first = seconds(7);
    d.query.last = seconds(11983);
    d.query.coverage = {-.01, .01, -.01, .01, .1, 0, true};
    std::vector<core::AccessInterval> independent;
    for (int i = 0; i < 65; ++i) {
        auto sat = seed;
        const auto angle = 2 * pi * double(i) / 65;
        const auto c = std::cos(angle), s = std::sin(angle);
        sat.orbit.object_id = "asset-" + std::to_string(i);
        sat.orbit.initial.position_m = {7e6 * c, 7e6 * s, 0};
        sat.orbit.initial.velocity_mps = {-6200 * s, 6200 * c, 5300};
        ctx.project.satellites.push_back(sat);
        d.query.object_ids.push_back(sat.orbit.object_id);
    }
    for (const auto &id : d.query.object_ids) {
        auto single = d;
        single.query.object_ids = {id};
        const auto result = runtime::generate_report(ctx, single);
        const auto &windows = result.coverage_statistics->points[0].windows;
        independent.insert(independent.end(), windows.begin(), windows.end());
    }
    const auto expected = core::merge_coverage_intervals(d.query.first, d.query.last, independent);
    ASSERT_FALSE(expected.empty());
    for (int order = 0; order < 2; ++order) {
        const auto result = runtime::generate_report(ctx, d);
        const auto &actual = result.coverage_statistics->points[0].windows;
        ASSERT_EQ(actual.size(), expected.size());
        for (std::size_t i = 0; i < expected.size(); ++i) {
            EXPECT_EQ(actual[i].first, expected[i].first);
            EXPECT_EQ(actual[i].last, expected[i].last);
            EXPECT_EQ(actual[i].start_clipped, expected[i].first == d.query.first);
            EXPECT_EQ(actual[i].stop_clipped, expected[i].last == d.query.last);
        }
        std::ranges::reverse(d.query.object_ids);
    }
}

TEST(Coverage, LongSearchReusesTransformsAcrossAssetsAndReportsMonotonicProgress) {
    if (!runtime::time_frames_available() || !runtime::two_body_available())
        GTEST_SKIP();
    auto ctx = context();
    auto frames = std::make_shared<IdentityFrames>();
    ctx.frames = frames;
    auto d = definition();
    d.query.follow_scenario = false;
    d.query.first = seconds(0);
    d.query.last = seconds(600);
    d.query.step_nanoseconds = 10'000'000;
    d.query.event_search_mode = core::AccessSearchMode::fixed_grid;
    d.query.coverage = {-.01, .01, 3.0, 3.1, .2, 0, true};
    std::size_t previous = 0, notifications = 0;
    const auto result =
        runtime::generate_report(ctx, d, nullptr, [&](std::size_t done, std::size_t total) {
            EXPECT_GE(done, previous);
            EXPECT_LE(done, total);
            previous = done;
            ++notifications;
        });
    ASSERT_EQ(result.coverage_statistics->points.size(), 1);
    EXPECT_TRUE(result.coverage_statistics->points[0].windows.empty());
    EXPECT_DOUBLE_EQ(result.coverage_statistics->maximum_gap_seconds, 600);
    EXPECT_GT(notifications, 0);
    EXPECT_EQ(previous, result.table.rows().size());
    // More epochs than the frame cache holds: evaluating each asset over the
    // entire interval first would repeat almost every expensive transform.
    const auto epochs = 60000u + 1;
    EXPECT_GE(frames->state_calls, epochs);
    EXPECT_LT(frames->state_calls, epochs + 100);
}

TEST(Coverage, DenseGridUsesAdjustableMemoryBudgetAndKeepsEveryPoint) {
    if (!runtime::time_frames_available() || !runtime::two_body_available())
        GTEST_SKIP();
    auto ctx = context();
    auto d = definition();
    d.query.object_ids = {"sat0"};
    d.query.follow_scenario = false;
    d.query.first = seconds(0);
    d.query.last = seconds(1);
    d.query.event_search_mode = core::AccessSearchMode::fixed_grid;
    d.query.coverage = {-pi / 2, pi / 2, -pi, pi, .5 * pi / 180, 0, true};
    const auto estimate = runtime::estimate_coverage_report(ctx.project, d);
    EXPECT_GT(estimate.points, 160000);
    EXPECT_EQ(estimate.rows, estimate.points);
    ctx.coverage_memory_bytes = 64 * 1024 * 1024;
    EXPECT_THROW((void)runtime::generate_report(ctx, d), std::invalid_argument);
    ctx.coverage_memory_bytes = runtime::default_coverage_memory_bytes;
    const auto result = runtime::generate_report(ctx, d);
    ASSERT_TRUE(result.coverage_statistics);
    EXPECT_EQ(result.table.rows().size(), estimate.points);
    EXPECT_EQ(result.coverage_statistics->points.size(), estimate.points);
    EXPECT_GT(result.coverage_statistics->mean_coverage_fraction, 0);
    EXPECT_LT(result.coverage_statistics->mean_coverage_fraction, 1);
    // A dense polygon preflight is cancellable even before allocating result rows.
    application::ProjectRegion region;
    region.object_id = "dense";
    region.display_name = "Dense";
    region.boundary.vertices = {{0, 0}, {.5, 0}, {0, .5}};
    region.grid_resolution_rad = 1e-6;
    ctx.project.regions.push_back(region);
    d.query.region_id = "dense";
    d.query.coverage.resolution_rad = 1e-6;
    EXPECT_THROW((void)runtime::estimate_coverage_report(ctx.project, d,
                                                         runtime::default_coverage_memory_bytes,
                                                         [] { throw runtime::ReportCancelled{}; }),
                 runtime::ReportCancelled);
}

TEST(Coverage, TimeCurveMatchesWindowMembershipAcrossBatchesAndPartialFinalStep) {
    if (!runtime::time_frames_available() || !runtime::two_body_available())
        GTEST_SKIP();
    auto ctx = context();
    auto d = definition();
    d.query.kind = application::ReportKind::coverage_time;
    d.query.follow_scenario = false;
    d.query.first = seconds(7);
    d.query.last = seconds(11983);
    d.query.step_nanoseconds = 37'000'000'000;
    d.query.coverage = {-.5, .5, -.5, .5, .25, 0, true};
    const auto result = runtime::generate_report(ctx, d);
    const auto &stats = *result.coverage_statistics;
    ASSERT_EQ(std::get<core::Epoch>(result.table.rows().back()[2]), d.query.last);
    for (const auto &row : result.table.rows()) {
        const auto t = std::get<core::Epoch>(row[2]).tai_nanoseconds();
        double visible = 0, accumulated = 0;
        for (const auto &point : stats.points) {
            if (!point.windows.empty() && point.windows.front().first.tai_nanoseconds() <= t)
                accumulated += point.cell.area_m2;
            for (const auto &window : point.windows)
                if (window.first.tai_nanoseconds() <= t && t <= window.last.tai_nanoseconds()) {
                    visible += point.cell.area_m2;
                    break;
                }
        }
        EXPECT_NEAR(std::get<double>(row[3]), visible, stats.total_area_m2 * 1e-14);
        EXPECT_NEAR(std::get<double>(row[5]), accumulated, stats.total_area_m2 * 1e-14);
    }
    // Output rows and search work can exceed the former fixed node ceiling.
    d.query.object_ids = {"sat0"};
    d.query.first = seconds(0);
    d.query.last = core::Epoch{199999500000};
    d.query.step_nanoseconds = 1000000;
    d.query.event_search_mode = core::AccessSearchMode::fixed_grid;
    d.query.coverage = {-.01, .01, -.01, .01, .1, 0, true};
    const auto dense = runtime::generate_report(ctx, d);
    EXPECT_EQ(dense.table.rows().size(), 200001);
    EXPECT_EQ(std::get<core::Epoch>(dense.table.rows().back()[2]), d.query.last);
    EXPECT_DOUBLE_EQ(std::get<double>(dense.table.rows().back()[4]), 1);
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

TEST(Coverage, NamedPolygonUsesCapturedGeometryAndMissingRegionNeverFallsBack) {
    if (!runtime::time_frames_available() || !runtime::two_body_available())
        GTEST_SKIP();
    auto ctx = context();
    application::ProjectRegion region;
    region.object_id = "area";
    region.display_name = "Triangle";
    region.boundary.vertices = {{-.1, -.1}, {.1, -.1}, {0, .1}};
    region.grid_resolution_rad = .05;
    ctx.project.regions.push_back(region);
    auto d = definition();
    d.query.region_id = "area";
    d.query.follow_scenario = false;
    d.query.first = seconds(0);
    d.query.last = seconds(60);
    d.query.coverage.resolution_rad = 1; // Inactive inline draft; named region owns the grid.
    const auto result = runtime::generate_report(ctx, d);
    const auto &stats = *result.coverage_statistics;
    ASSERT_FALSE(stats.points.empty());
    ASSERT_TRUE(stats.region);
    EXPECT_EQ(*stats.region, region.boundary);
    EXPECT_NEAR(stats.grid.west_rad, -.1, 1e-15);
    EXPECT_DOUBLE_EQ(stats.grid.resolution_rad, .05);
    EXPECT_EQ(runtime::estimate_report_rows(ctx.project, d), stats.points.size());
    for (const auto &point : stats.points)
        EXPECT_TRUE(core::region_contains(region.boundary, {point.cell.location.longitude_rad,
                                                            point.cell.location.latitude_rad}));
    const auto report_hash = runtime::report_input_sha256(ctx.project);
    const auto orbit_hash = application::analysis_input_sha256(ctx.project);
    ctx.project.regions[0].display_name = "Renamed";
    ctx.project.regions[0].description = "A note";
    ctx.project.regions[0].display.color = 0xffffff;
    ctx.project.regions[0].display.fill = false;
    ctx.project.regions[0].display.opacity = .37;
    EXPECT_EQ(runtime::report_input_sha256(ctx.project), report_hash);
    ctx.project.regions[0].grid_resolution_rad = .025;
    EXPECT_NE(runtime::report_input_sha256(ctx.project), report_hash);
    EXPECT_EQ(application::analysis_input_sha256(ctx.project), orbit_hash);
    EXPECT_DOUBLE_EQ(stats.grid.resolution_rad, .05);
    ctx.project.regions[0].boundary.vertices[2].latitude_rad = .2;
    EXPECT_NE(runtime::report_input_sha256(ctx.project), report_hash);
    EXPECT_EQ(application::analysis_input_sha256(ctx.project), orbit_hash);
    EXPECT_EQ(*stats.region, region.boundary); // Already published results own their boundary.
    const auto reopened = application::decode_project(application::encode_project(ctx.project));
    EXPECT_EQ(reopened.regions.front().boundary, ctx.project.regions.front().boundary);
    EXPECT_DOUBLE_EQ(reopened.regions.front().grid_resolution_rad, .025);
    EXPECT_EQ(reopened.regions.front().display, ctx.project.regions.front().display);
    ctx.project.regions.clear();
    EXPECT_THROW((void)runtime::generate_report(ctx, d), application::ProjectError);
    EXPECT_THROW((void)runtime::estimate_report_rows(ctx.project, d), application::ProjectError);
}
