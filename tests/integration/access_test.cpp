#include <cmath>
#include <fstream>
#include <gtest/gtest.h>
#include <limits>
#include <numbers>
#include <orbitforge/runtime/access.hpp>
#include <orbitforge/runtime/project.hpp>
#include <orbitforge/runtime/time_frames.hpp>
#include <orbitforge/runtime/two_body.hpp>
#include <sstream>

using namespace orbitforge;
namespace {
constexpr double pi = std::numbers::pi;
core::Epoch seconds(double value) {
    return core::Epoch{static_cast<std::int64_t>(std::llround(value * 1e9))};
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
runtime::ReportContext circular() {
    application::Project p{"Access", seconds(0), seconds(5000)};
    application::ProjectSatellite sat;
    sat.orbit.first = p.first;
    sat.orbit.last = p.last;
    sat.orbit.initial = {p.first,
                         core::OriginId::earth,
                         core::FrameId::gcrf,
                         {7e6, 0, 0},
                         {0, std::sqrt(sat.orbit.mu() / 7e6), 0}};
    p.satellites.push_back(sat);
    p.ground_stations.push_back({"station", "Equator", {0, 0, 0}});
    auto eop = runtime::load_eop_snapshot(ORBITFORGE_REPORT_EOP);
    p.earth_orientation = eop->data_info();
    return {p, runtime::make_builtin_time_provider(), std::make_shared<IdentityFrames>(),
            std::move(eop)};
}
application::ReportDefinition report(application::ReportKind kind) {
    application::ReportDefinition d;
    d.id = d.name = "access";
    d.query.kind = kind;
    d.query.object_ids = {"station", "satellite-1"};
    d.query.step_nanoseconds = 10'000'000'000;
    return d;
}
} // namespace
TEST(Access, EnuAnglesRangeRateAndEllipsoidOcclusion) {
    core::CartesianState state{
        seconds(0), core::OriginId::earth, core::FrameId::itrf, {6378137 + 1000, 0, 0}, {12, 3, 4}};
    auto result = core::observe_ground_target({}, state);
    EXPECT_FALSE(result.azimuth_rad);
    EXPECT_NEAR(result.elevation_rad, pi / 2, 1e-14);
    EXPECT_DOUBLE_EQ(result.range_m, 1000);
    EXPECT_DOUBLE_EQ(result.range_rate_mps, 12);
    EXPECT_TRUE(result.line_of_sight);
    state.position_m = {6378137, 1000, 1000};
    result = core::observe_ground_target({}, state);
    ASSERT_TRUE(result.azimuth_rad);
    EXPECT_NEAR(*result.azimuth_rad, pi / 4, 1e-14);
    EXPECT_NEAR(result.elevation_rad, 0, 1e-14);
    EXPECT_TRUE(result.line_of_sight);
    state.position_m = {-7e6, 0, 0};
    EXPECT_FALSE(core::observe_ground_target({}, state).line_of_sight);
    state.position_m = {7e6, 0, 0};
    EXPECT_FALSE(core::observe_ground_target({0, 0, -100}, state).line_of_sight);
    state.frame = core::FrameId::gcrf;
    EXPECT_THROW((void)core::observe_ground_target({}, state), std::invalid_argument);
    EXPECT_NEAR(core::vector_angle({1, 0, 0}, {0, 1, 0}), pi / 2, 1e-14);
    EXPECT_THROW((void)core::vector_angle({0, 0, 0}, {1, 0, 0}), std::invalid_argument);
}
TEST(Access, RefinesTransitionsClipsEdgesAndDocumentsDiscoveryLimit) {
    const auto windows =
        core::find_access_intervals(seconds(-1), seconds(20), 3'000'000'000, [](core::Epoch t) {
            const auto s = t.seconds_since(seconds(0));
            return s < 2.123456 || s > 7.654321;
        });
    ASSERT_EQ(windows.size(), 2U);
    EXPECT_TRUE(windows[0].start_clipped);
    EXPECT_NEAR(windows[0].last.seconds_since(seconds(0)), 2.123456, .001);
    EXPECT_NEAR(windows[1].first.seconds_since(seconds(0)), 7.654321, .001);
    EXPECT_TRUE(windows[1].stop_clipped);
    EXPECT_TRUE(
        core::find_access_intervals(seconds(0), seconds(10), 10'000'000'000, [](core::Epoch t) {
            auto s = t.seconds_since(seconds(0));
            return s > 3 && s < 4;
        }).empty());
    EXPECT_THROW(
        (void)core::find_access_intervals(seconds(0), seconds(10), 0, [](auto) { return true; }),
        std::invalid_argument);
    EXPECT_THROW(
        (void)core::find_access_intervals(seconds(0), seconds(10), 1, [](auto) { return true; }),
        std::invalid_argument);
    EXPECT_TRUE(core::find_access_intervals(seconds(0), seconds(0), 1'000'000, [](auto) {
                    return true;
                }).empty());
}
TEST(Access, CircularHorizonMatchesAnalyticSolutionAndConeNarrowsWindow) {
    if (!runtime::time_frames_available() || !runtime::two_body_available())
        GTEST_SKIP();
    auto context = circular();
    auto d = report(application::ReportKind::access_intervals);
    const auto result = runtime::generate_report(context, d);
    ASSERT_EQ(result.table.rows().size(), 1U);
    const double rate = std::sqrt(context.project.satellites[0].orbit.mu() / std::pow(7e6, 3));
    const double horizon = std::acos(6378137.0 / 7e6) / rate;
    EXPECT_NEAR(std::get<core::Epoch>(result.table.rows()[0][3]).seconds_since(seconds(0)), horizon,
                .001);
    context.project.sensors.push_back(
        {"sensor", "Nadir", "satellite-1", 10 * pi / 180, application::SensorPointing::nadir, {}});
    d.query.object_ids[1] = "sensor";
    const auto narrow = runtime::generate_report(context, d);
    ASSERT_EQ(narrow.table.rows().size(), 1U);
    const double cone = 10 * pi / 180;
    const double central_angle = std::asin(7e6 / 6378137.0 * std::sin(cone)) - cone;
    EXPECT_NEAR(std::get<double>(narrow.table.rows()[0][4]), central_angle / rate, .001);
    context.project.sensors[0].pointing = application::SensorPointing::targeted;
    context.project.sensors[0].target_station_id = "station";
    const auto tracking = runtime::generate_report(context, d);
    ASSERT_EQ(tracking.table.rows().size(), 1U);
    EXPECT_DOUBLE_EQ(std::get<double>(tracking.table.rows()[0][4]),
                     std::get<double>(result.table.rows()[0][4]));
}
TEST(Access, SnapshotValidationAerAndCancellation) {
    if (!runtime::time_frames_available() || !runtime::two_body_available())
        GTEST_SKIP();
    auto context = circular();
    runtime::AccessSession access{context, "station", "satellite-1"};
    context.project.ground_stations[0].minimum_elevation_rad = pi / 2;
    EXPECT_TRUE(access.at(seconds(10)).visible); // Owns the original snapshot.
    EXPECT_THROW((void)access.at(seconds(-1)), std::out_of_range);
    auto d = report(application::ReportKind::aer);
    d.query.follow_scenario = false;
    d.query.first = seconds(0);
    d.query.last = seconds(10);
    const auto result = runtime::generate_report(context, d);
    ASSERT_EQ(result.table.rows().size(), 2U);
    EXPECT_TRUE(std::holds_alternative<std::monostate>(result.table.rows()[0][3]));
    EXPECT_EQ(std::get<std::int64_t>(result.table.rows()[1][7]), 0);
    std::atomic_bool cancelled{true};
    EXPECT_THROW((void)runtime::generate_report(context, d, &cancelled), runtime::ReportCancelled);
    context.eop.reset();
    EXPECT_THROW((void)runtime::generate_report(context, d), std::invalid_argument);
}
TEST(Access, RangeRateIncludesRotatingEarthAndProvidersMustMatch) {
    if (!runtime::time_frames_available() || !runtime::two_body_available())
        GTEST_SKIP();
    auto context = circular();
    context.frames = runtime::make_frame_provider();
    runtime::AccessSession access{context, "station", "satellite-1"};
    const auto sample = access.at(seconds(100));
    const double derivative = (access.at(seconds(100.01)).observation.range_m -
                               access.at(seconds(99.99)).observation.range_m) /
                              .02;
    // Central difference over 20 ms: < 1 mm/s including Earth's rotation.
    EXPECT_NEAR(sample.observation.range_rate_mps, derivative, .001);
    context.project.earth_orientation->sha256[0] =
        context.project.earth_orientation->sha256[0] == '0' ? '1' : '0';
    EXPECT_THROW((runtime::AccessSession{context, "station", "satellite-1"}),
                 std::invalid_argument);
}
TEST(Access, SensorPersistenceCopyDeletionAndReferenceValidation) {
    application::Project p{"Sensor", seconds(0), seconds(7200)};
    p.satellites.push_back({});
    p.ground_stations.push_back({"station", "Station", {}, .2});
    p.sensors.push_back(
        {"sensor", "Cone", "satellite-1", .1, application::SensorPointing::targeted, "station"});
    auto decoded = application::decode_project(application::encode_project(p));
    ASSERT_EQ(decoded.sensors.size(), 1U);
    EXPECT_EQ(decoded.sensors[0].target_station_id, "station");
    EXPECT_DOUBLE_EQ(decoded.ground_stations[0].minimum_elevation_rad, .2);
    EXPECT_THROW((void)p.without_ground_station("station"), application::ProjectError);
    EXPECT_TRUE(p.without_satellite("satellite-1").sensors.empty());
    auto copy = p.with_satellite_copy("satellite-1", "copy", "Copy");
    ASSERT_EQ(copy.sensors.size(), 2U);
    EXPECT_EQ(copy.sensors[1].parent_satellite_id, "copy");
    EXPECT_NE(copy.sensors[1].object_id, "sensor");
    decoded.sensors[0].parent_satellite_id = "missing";
    EXPECT_THROW(application::validate_project(decoded), application::ProjectError);
}

TEST(Access, ExtremumDiscoveryFindsShortOffGridPassAndHiddenGap) {
    for (const auto centre : {.123456, 3.14159, 9.87654}) {
        const auto margin = [=](core::Epoch t) {
            const auto d = t.seconds_since(seconds(0)) - centre;
            return .0001 - d * d; // 20 ms pass, including near check-cell endpoints.
        };
        const auto windows =
            core::find_constraint_intervals(seconds(0), seconds(10), 10'000'000'000, margin);
        ASSERT_EQ(windows.size(), 1U);
        EXPECT_NEAR(windows[0].first.seconds_since(seconds(0)), centre - .01, .001);
        EXPECT_NEAR(windows[0].last.seconds_since(seconds(0)), centre + .01, .001);
        const auto gaps = core::find_constraint_intervals(seconds(0), seconds(10), 10'000'000'000,
                                                          [&](auto t) { return -margin(t); });
        ASSERT_EQ(gaps.size(), 2U);
        EXPECT_TRUE(gaps[0].start_clipped);
        EXPECT_TRUE(gaps[1].stop_clipped);
        EXPECT_NEAR(gaps[0].last.seconds_since(seconds(0)), centre - .01, .001);
        EXPECT_NEAR(gaps[1].first.seconds_since(seconds(0)), centre + .01, .001);
    }
}
TEST(Access, ExtremumSearchRejectsInvalidValuesAndPreservesCancellation) {
    const auto f = [](core::Epoch t) { return -std::pow(t.seconds_since(seconds(0)) - 5, 2); };
    EXPECT_TRUE(
        core::find_constraint_intervals(seconds(0), seconds(10), 10'000'000'000, f).empty());
    EXPECT_TRUE(core::find_constraint_intervals(seconds(0), seconds(0), 1'000'000, f).empty());
    EXPECT_THROW((void)core::find_constraint_intervals(
                     seconds(0), seconds(1), 1'000'000,
                     [](auto) { return std::numeric_limits<double>::quiet_NaN(); }),
                 std::invalid_argument);
    EXPECT_THROW(
        (void)core::find_constraint_intervals(seconds(0), seconds(1), 1'000'000, f, 2'000'000),
        std::invalid_argument);
    int calls = 0;
    EXPECT_THROW((void)core::find_constraint_intervals(seconds(0), seconds(10), 10'000'000'000,
                                                       [&](auto t) {
                                                           if (++calls == 8)
                                                               throw runtime::ReportCancelled{};
                                                           return f(t);
                                                       }),
                 runtime::ReportCancelled);
    const auto flat = core::find_access_extremum(
        seconds(0), seconds(1), 10'000'000'000, [](auto) { return 12.; }, true);
    EXPECT_EQ(flat.epoch, seconds(0));
    EXPECT_EQ(flat.value, 12.);
    const core::Epoch near_limit{-9'000'000'000'000'000'000};
    const auto extreme = core::find_access_extremum(
        near_limit, core::Epoch{near_limit.tai_nanoseconds() + 10'000'000'000}, 10'000'000'000,
        [&](auto t) { return std::pow(t.seconds_since(near_limit) - 3.25, 2); }, false);
    EXPECT_NEAR(extreme.epoch.seconds_since(near_limit), 3.25, .001);
}
TEST(Access, PassesExtremaAndStatisticsMatchOrekitHipparchus) {
    if (!runtime::time_frames_available() || !runtime::two_body_available())
        GTEST_SKIP();
    double max_boundary_error = 0, max_elevation_error = 0, max_range_error = 0,
           max_extremum_time_error = 0;
    std::ifstream file(ORBITFORGE_ACCESS_REFERENCE);
    ASSERT_TRUE(file);
    // Independent fixed reference, generated by Orekit events and Hipparchus Brent.
    // Run two check spacings to establish discovery convergence for these cases.
    for (const auto step : {60'000'000'000LL, 30'000'000'000LL}) {
        file.clear();
        file.seekg(0);
        std::string line, name;
        std::vector<results::AccessWindow> actual;
        std::size_t row = 0, cases = 0;
        results::AccessQuery query{seconds(0), seconds(1), std::chrono::nanoseconds{step},
                                   std::chrono::nanoseconds{1000}};
        while (std::getline(file, line)) {
            std::istringstream in(line);
            char type;
            in >> type;
            if (type == 'C') {
                double first, last, vy, vz, lon, lat, elevation, cone;
                in >> name >> first >> last >> vy >> vz >> lon >> lat >> elevation >> cone;
                SCOPED_TRACE(name);
                auto context = circular();
                auto &p = context.project;
                p.first = seconds(first);
                p.last = seconds(last);
                p.current = p.first;
                p.satellites[0].orbit.initial.velocity_mps = {0, vy, vz};
                p.satellites[0].orbit.first = p.first;
                p.satellites[0].orbit.last = p.last;
                p.ground_stations[0].location = {lon, lat, 0};
                p.ground_stations[0].minimum_elevation_rad = elevation;
                std::string target = "satellite-1";
                if (cone > 0) {
                    target = "sensor";
                    p.sensors.push_back({target,
                                         "Cone",
                                         "satellite-1",
                                         cone,
                                         application::SensorPointing::nadir,
                                         {}});
                }
                runtime::AccessSession session(context, "station", target);
                query.first = p.first;
                query.last = p.last;
                actual = runtime::calculate_access_passes(p, {"station", target}, query,
                                                          [&](auto t) { return session.at(t); });
                row = 0;
                ++cases;
            } else if (type == 'P') {
                SCOPED_TRACE(name + " pass " + std::to_string(row));
                ASSERT_LT(row, actual.size());
                double first, last, el_t, el, range_t, range;
                int a, b;
                in >> first >> last >> a >> b >> el_t >> el >> range_t >> range;
                const auto &w = actual[row++];
                ASSERT_TRUE(w.extrema());
                EXPECT_NEAR(w.start().seconds_since(seconds(0)), first, 1.1e-6);
                EXPECT_NEAR(w.end().seconds_since(seconds(0)), last, 1.1e-6);
                EXPECT_EQ(w.start_clipped(), a != 0);
                EXPECT_EQ(w.end_clipped(), b != 0);
                const auto &e = *w.extrema();
                max_boundary_error = std::max(
                    {max_boundary_error, std::abs(w.start().seconds_since(seconds(0)) - first),
                     std::abs(w.end().seconds_since(seconds(0)) - last)});
                max_elevation_error =
                    std::max(max_elevation_error, std::abs(e.maximum_elevation.value - el));
                max_range_error =
                    std::max(max_range_error, std::abs(e.minimum_range.value - range));
                max_extremum_time_error =
                    std::max({max_extremum_time_error,
                              std::abs(e.maximum_elevation.epoch.seconds_since(seconds(0)) - el_t),
                              std::abs(e.minimum_range.epoch.seconds_since(seconds(0)) - range_t)});
                EXPECT_NEAR(e.maximum_elevation.value, el, 3e-8);
                EXPECT_NEAR(e.minimum_range.value, range, .01);
                // Objective rounding makes nearly flat stationary times less identifiable
                // than event crossings, particularly at zenith; use a 50 us epoch bound.
                EXPECT_NEAR(e.maximum_elevation.epoch.seconds_since(seconds(0)), el_t, 5e-5);
                EXPECT_NEAR(e.minimum_range.epoch.seconds_since(seconds(0)), range_t, 5e-5);
            } else if (type == 'S') {
                SCOPED_TRACE(name);
                std::size_t count;
                double total, mean, shortest, longest;
                in >> count >> total >> mean >> shortest >> longest;
                EXPECT_EQ(row, count);
                ASSERT_EQ(actual.size(), count);
                const auto stats = results::AccessWindows{actual, query, {}}.statistics();
                EXPECT_EQ(stats.count, count);
                EXPECT_NEAR(stats.total_duration_s, total, 2.2e-6 * static_cast<double>(count + 1));
                EXPECT_NEAR(stats.mean_duration_s, mean, 2.2e-6);
                EXPECT_NEAR(stats.shortest_duration_s, shortest, 2.2e-6);
                EXPECT_NEAR(stats.longest_duration_s, longest, 2.2e-6);
            } else
                FAIL() << "Unknown reference row";
            ASSERT_FALSE(in.fail());
        }
        EXPECT_EQ(cases, 7U);
    }
    std::ostringstream metrics;
    metrics.precision(17);
    metrics << "boundary_s=" << max_boundary_error << "; elevation_rad=" << max_elevation_error
            << "; range_m=" << max_range_error << "; extremum_epoch_s=" << max_extremum_time_error;
    RecordProperty("orekit_max_errors", metrics.str());
}
TEST(Access, SearchSettingsRoundTripAndInvalidModesAreRejected) {
    if (!runtime::time_frames_available() || !runtime::two_body_available())
        GTEST_SKIP();
    auto context = circular();
    auto d = report(application::ReportKind::access_intervals);
    d.query.access_search_mode = core::AccessSearchMode::fixed_grid;
    d.query.access_tolerance_nanoseconds = 12345;
    context.project.reports.push_back(d);
    const auto decoded = application::decode_project(application::encode_project(context.project));
    EXPECT_EQ(decoded.reports[0].query, d.query);
    d.query.access_tolerance_nanoseconds = d.query.step_nanoseconds + 1;
    EXPECT_THROW(application::validate_report_definition(d), std::invalid_argument);
}
