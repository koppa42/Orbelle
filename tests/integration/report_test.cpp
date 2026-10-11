#include <cmath>
#include <fstream>
#include <gtest/gtest.h>
#include <limits>
#include <orbitforge/runtime/project.hpp>
#include <orbitforge/runtime/report.hpp>
#include <orbitforge/runtime/time_frames.hpp>
#include <orbitforge/runtime/two_body.hpp>
#include <random>

using namespace orbitforge;
namespace {
runtime::ReportContext stations(std::int64_t last = 2'000'000'001) {
    application::Project p("Reports", core::Epoch{0}, core::Epoch{last});
    p.ground_stations.push_back({"station", "海岸,\"一号\"\n站", {0, 0, 0}});
    return {p, {}, {}, {}};
}
application::ReportDefinition
definition(application::ReportKind kind = application::ReportKind::cartesian_state) {
    application::ReportDefinition d;
    d.id = "report-a";
    d.name = "报告";
    d.kind = kind;
    d.frame = core::FrameId::itrf;
    d.time_scale = application::ReportTimeScale::tai;
    d.step_nanoseconds = 1'000'000'000;
    return d;
}
std::string read(const std::filesystem::path &path) {
    std::ifstream input(path, std::ios::binary);
    return {std::istreambuf_iterator<char>{input}, {}};
}
} // namespace
TEST(Reports, SamplesIntegerNanosecondsAndIncludesExactStopOnce) {
    auto c = stations();
    auto d = definition();
    const auto result = runtime::generate_report(c, d);
    ASSERT_EQ(result.table.rows().size(), 4U);
    const std::array<std::int64_t, 4> expected{0, 1'000'000'000, 2'000'000'000, 2'000'000'001};
    for (std::size_t i = 0; i < expected.size(); ++i) {
        EXPECT_EQ(std::get<core::Epoch>(result.table.rows()[i][2]).tai_nanoseconds(), expected[i]);
        EXPECT_DOUBLE_EQ(std::get<double>(result.table.rows()[i][4]), 6378137);
        EXPECT_DOUBLE_EQ(std::get<double>(result.table.rows()[i][7]), 0);
    }
    EXPECT_EQ(result.cell_text(3, 2), "2000-01-01T12:00:02.000000001");
    d.follow_scenario = false;
    d.first = core::Epoch{1};
    d.last = core::Epoch{1};
    EXPECT_EQ(runtime::generate_report(c, d).table.rows().size(), 1U);
    d.first = c.project.first;
    d.last = core::Epoch{2'000'000'000};
    EXPECT_EQ(runtime::generate_report(c, d).table.rows().size(), 3U);
}
TEST(Reports, StaticTemplatesHaveTypedValuesAndDoNotRequireOrbitLibraries) {
    auto c = stations();
    auto summary =
        runtime::generate_report(c, definition(application::ReportKind::scenario_summary));
    ASSERT_EQ(summary.table.rows().size(), 7U);
    EXPECT_EQ(std::get<std::int64_t>(summary.table.rows()[5][1]), 1);
    auto objects = runtime::generate_report(c, definition(application::ReportKind::object_summary));
    EXPECT_EQ(std::get<std::string>(objects.table.rows()[0][3]), "WGS84 fixed");
    const auto geo =
        runtime::generate_report(c, definition(application::ReportKind::ground_stations));
    EXPECT_EQ(geo.table.columns()[2].unit, "rad");
    EXPECT_EQ(geo.column_title(2), "Longitude (deg)");
    EXPECT_EQ(geo.column_title(5), "ITRF X (km)");
    EXPECT_DOUBLE_EQ(std::stod(geo.cell_text(0, 5)), 6378.137);
}
TEST(Reports, IndependentScientificSamplesMatchSessionRegardlessOfDisplayStep) {
    if (!runtime::two_body_available())
        GTEST_SKIP();
    auto c = stations(120'000'000'000);
    application::ProjectSatellite satellite;
    satellite.orbit.first = c.project.first;
    satellite.orbit.last = c.project.last;
    satellite.orbit.step_seconds = 60;
    c.project.satellites.push_back(satellite);
    auto d = definition();
    d.frame = core::FrameId::gcrf;
    d.object_ids = {satellite.orbit.object_id};
    d.step_nanoseconds = 13'000'000'001;
    const auto result = runtime::generate_report(c, d);
    const auto session = runtime::make_orbit_session(satellite.orbit);
    for (const auto &row : result.table.rows()) {
        const auto epoch = std::get<core::Epoch>(row[2]);
        const auto expected = session.sample(std::array{epoch}).states()[0];
        for (std::size_t axis = 0; axis < 3; ++axis) {
            EXPECT_DOUBLE_EQ(std::get<double>(row[4 + axis]), expected.position_m[axis]);
            EXPECT_DOUBLE_EQ(std::get<double>(row[7 + axis]), expected.velocity_mps[axis]);
        }
    }
    c.project.satellites[0].orbit.step_seconds = 1;
    EXPECT_EQ(runtime::generate_report(c, d).input_sha256, result.input_sha256);
}
TEST(Reports, FrameConversionRetainsEarthRotationVelocityAndConfirmedData) {
    if (!runtime::time_frames_available())
        GTEST_SKIP();
    auto c = stations();
    c.time = runtime::make_builtin_time_provider();
    c.frames = runtime::make_frame_provider();
    c.eop = runtime::load_eop_snapshot(ORBITFORGE_REPORT_EOP);
    c.project.earth_orientation = c.eop->data_info();
    auto d = definition();
    d.frame = core::FrameId::gcrf;
    const auto result = runtime::generate_report(c, d);
    const auto fixed =
        core::GroundStation(c.project.ground_stations[0].location).state_at(c.project.first);
    const auto expected =
        c.frames->gcrf_to_itrf_state(fixed.epoch, c.eop->at(fixed.epoch)).inverse(fixed);
    double speed = 0;
    for (std::size_t axis = 0; axis < 3; ++axis) {
        EXPECT_DOUBLE_EQ(std::get<double>(result.table.rows()[0][4 + axis]),
                         expected.position_m[axis]);
        EXPECT_DOUBLE_EQ(std::get<double>(result.table.rows()[0][7 + axis]),
                         expected.velocity_mps[axis]);
        speed += expected.velocity_mps[axis] * expected.velocity_mps[axis];
    }
    EXPECT_GT(std::sqrt(speed), 400);
    EXPECT_EQ(result.data_sha256, c.project.data_lock().sha256());
    c.project.earth_orientation->sha256[0] =
        c.project.earth_orientation->sha256[0] == '0' ? '1' : '0';
    EXPECT_THROW((void)runtime::generate_report(c, d), std::invalid_argument);
    c.project.earth_orientation = c.eop->data_info();
    c.project.earth_orientation->time_data.sha256[0] = 'x';
    EXPECT_THROW((void)runtime::generate_report(c, d), std::exception);
}
TEST(Reports, UtcLeapSecondTaiAndTtFormattingPreserveEpoch) {
    if (!runtime::time_frames_available())
        GTEST_SKIP();
    auto c = stations();
    c.time = runtime::make_builtin_time_provider();
    c.project = c.project.with_settings("Leap", c.time->from_utc({2016, 12, 31, 23, 59, 59, 0}),
                                        c.time->from_utc({2017, 1, 1, 0, 0, 0, 0}));
    auto d = definition();
    d.time_scale = application::ReportTimeScale::utc;
    auto r = runtime::generate_report(c, d);
    ASSERT_EQ(r.table.rows().size(), 3U);
    EXPECT_EQ(r.cell_text(1, 2), "2016-12-31T23:59:60.000000000Z");
    EXPECT_EQ(r.cell_text(2, 2), "2017-01-01T00:00:00.000000000Z");
    d.time_scale = application::ReportTimeScale::tt;
    r = runtime::generate_report(stations(), d);
    EXPECT_EQ(r.cell_text(0, 2), "2000-01-01T12:00:32.184000000");
}
TEST(Reports, RejectsMissingObjectsIncompatibleBindingsInvalidRangesAndExcessRows) {
    auto c = stations();
    auto d = definition();
    d.object_ids = {"missing"};
    EXPECT_THROW((void)runtime::generate_report(c, d), std::invalid_argument);
    d.object_ids.clear();
    d.follow_scenario = false;
    d.first = core::Epoch{-1};
    d.last = c.project.last;
    EXPECT_THROW((void)runtime::estimate_report_rows(c.project, d), std::invalid_argument);
    d.follow_scenario = true;
    d.step_nanoseconds = 1;
    EXPECT_THROW((void)runtime::estimate_report_rows(c.project, d), std::invalid_argument);
    d.step_nanoseconds = 0;
    EXPECT_THROW((void)runtime::estimate_report_rows(c.project, d), std::invalid_argument);
    d = definition();
    c.project.ground_stations.clear();
    EXPECT_THROW((void)runtime::generate_report(c, d), std::invalid_argument);
}
TEST(Reports, CancellationIsObservedBetweenBatchesWithoutPublishingPartialResult) {
    auto c = stations(2000'000'000'000);
    auto d = definition();
    std::atomic_bool cancel = false;
    std::size_t seen = 0;
    EXPECT_THROW((void)runtime::generate_report(c, d, &cancel,
                                                [&](std::size_t count, std::size_t total) {
                                                    EXPECT_EQ(total, 2001U);
                                                    seen = count;
                                                    if (count >= 512)
                                                        cancel.store(true);
                                                }),
                 runtime::ReportCancelled);
    EXPECT_EQ(seen, 512U);
    EXPECT_EQ(runtime::generate_report(c, d).table.rows().size(), 2001U);
}
TEST(Reports, CsvQuotesUnicodeAndPreservesExistingFilesOnFailureAndCancellation) {
    const auto path = std::filesystem::temp_directory_path() /
                      ("orbitforge-report-test-" + std::to_string(std::random_device{}()) + ".csv");
    const auto result = runtime::generate_report(stations(), definition());
    runtime::export_report_csv(result, path);
    const auto original = read(path);
    EXPECT_TRUE(original.starts_with("\xEF\xBB\xBF\"Object ID\""));
    EXPECT_NE(original.find("\"海岸,\"\"一号\"\"\n站\""), std::string::npos);
    EXPECT_NE(original.find("\"Time (TAI)\""), std::string::npos);
    EXPECT_THROW(runtime::export_report_csv(result, path), std::invalid_argument);
    std::atomic_bool cancel = true;
    EXPECT_THROW(runtime::export_report_csv(result, path, true, &cancel), runtime::ReportCancelled);
    EXPECT_EQ(read(path), original);
    runtime::export_report_csv(result, path, true);
    EXPECT_EQ(read(path), original);
    std::filesystem::remove(path);
}
TEST(Reports, SavedDefinitionsRoundTripAndMissingBindingsDoNotPreventProjectLoading) {
    auto p = stations().project;
    auto d = definition();
    d.object_ids = {"deleted-station"};
    d.follow_scenario = false;
    d.first = core::Epoch{17};
    d.last = core::Epoch{19};
    d.step_nanoseconds = 3;
    d.time_scale = application::ReportTimeScale::tt;
    p.reports.push_back(d);
    const auto bytes = application::encode_project(p);
    EXPECT_EQ(bytes[8], 6);
    const auto restored = application::decode_project(bytes);
    ASSERT_EQ(restored.reports.size(), 1U);
    EXPECT_EQ(application::encode_project(restored), bytes);
    EXPECT_EQ(restored.reports[0].object_ids, d.object_ids);
    EXPECT_THROW((void)runtime::estimate_report_rows(restored, restored.reports[0]),
                 std::invalid_argument);
    p.reports.push_back(d);
    EXPECT_THROW((void)application::encode_project(p), application::ProjectError);
}
TEST(Reports, ProvenanceIgnoresPlaybackAndConfigurationButDetectsNumericalChanges) {
    auto p = stations().project;
    const auto hash = runtime::report_input_sha256(p);
    p.current = core::Epoch{1};
    p.reports.push_back(definition());
    EXPECT_EQ(runtime::report_input_sha256(p), hash);
    p.ground_stations[0].location.height_m = 10;
    EXPECT_NE(runtime::report_input_sha256(p), hash);
    auto c = stations();
    auto r = runtime::generate_report(c, definition());
    c.project.ground_stations[0].location.height_m = 100;
    EXPECT_EQ(r.input_sha256, hash);
    EXPECT_DOUBLE_EQ(std::get<double>(r.table.rows()[0][4]), 6378137);
}
TEST(Reports, TablesRejectNonFiniteDataAndMismatchedColumnCounts) {
    EXPECT_THROW(
        (void)(results::Table{{{"x", "X", "m"}}, {{std::numeric_limits<double>::quiet_NaN()}}}),
        std::invalid_argument);
    EXPECT_THROW((void)(results::Table{{{"x", "X", "m"}}, {{1.0, 2.0}}}), std::invalid_argument);
    EXPECT_THROW((void)(results::Table{{{"x", "X", "m"}, {"x", "Y", "m"}}, {}}),
                 std::invalid_argument);
}
