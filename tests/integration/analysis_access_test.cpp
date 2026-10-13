#include <cmath>
#include <gtest/gtest.h>
#include <limits>
#include <orbitforge/runtime/access.hpp>
#include <orbitforge/runtime/analysis.hpp>
#include <orbitforge/runtime/time_frames.hpp>
#include <orbitforge/runtime/two_body.hpp>

using namespace orbitforge;
using namespace std::chrono_literals;
namespace app = orbitforge::application;
namespace {
app::OrbitInput circular() {
    app::OrbitInput orbit;
    orbit.initial.velocity_mps = {0, std::sqrt(orbit.mu() / 7e6), 0};
    return orbit;
}
struct Scene {
    runtime::DataSnapshot data = runtime::DataSnapshot::load(ORBITFORGE_REPORT_EOP);
    app::ProjectDocument document{
        app::Project{"Access", core::Epoch{0}, core::Epoch{7'200'000'000'000}}};
    app::ObjectRef satellite =
        document.objects().create(app::objects::Satellite{"Observer", circular(), {}});
    app::ObjectRef station =
        document.objects().create(app::objects::GroundStation{"Station", location()});
    app::ObjectRef sensor =
        document.objects().create(app::objects::Sensor{"Camera", satellite, .15, {}});

    core::GroundStationLocation location() const {
        const auto r = runtime::prepare(document, data)
                           .state(satellite, core::Epoch{0}, core::FrameId::itrf)
                           .position_m;
        return {std::atan2(r[1], r[0]), std::atan2(r[2], std::hypot(r[0], r[1])), 0};
    }
};
class AnalysisAccess : public testing::Test {
  protected:
    std::unique_ptr<Scene> scene;
    void SetUp() override {
        if (!runtime::time_frames_available() || !runtime::two_body_available())
            GTEST_SKIP();
        scene = std::make_unique<Scene>();
    }
};
} // namespace
TEST_F(AnalysisAccess, MatchesReportsAndSensorConstraints) {
    const auto &s = *scene;
    const auto analysis = runtime::prepare(s.document, s.data);
    auto raw = *s.document.snapshot().project;
    raw.earth_orientation = s.data.eop()->data_info();
    const runtime::ReportContext context{raw, s.data.time(), s.data.frames(), s.data.eop()};
    for (const auto &target : {s.satellite, s.sensor}) {
        runtime::AccessSession legacy{context, s.station.id(), target.id()};
        const auto expected = core::find_access_intervals(
            raw.first, raw.last, 10'000'000'000, [&](auto t) { return legacy.at(t).visible; });
        const auto actual = analysis.access(s.station, target, {.search_step = 10s});
        ASSERT_FALSE(actual.windows().empty());
        ASSERT_EQ(actual.windows().size(), expected.size());
        for (std::size_t i = 0; i < expected.size(); ++i) {
            const auto &w = actual.windows()[i];
            EXPECT_EQ(w.start(), expected[i].first);
            EXPECT_EQ(w.end(), expected[i].last);
            EXPECT_EQ(w.start_clipped(), expected[i].start_clipped);
            EXPECT_EQ(w.end_clipped(), expected[i].stop_clipped);
            EXPECT_EQ(w.duration().count(),
                      w.end().tai_nanoseconds() - w.start().tai_nanoseconds());
            EXPECT_TRUE(legacy.at(w.start()).visible);
            EXPECT_TRUE(legacy.at(w.end()).visible);
            if (!w.end_clipped()) {
                EXPECT_FALSE(legacy.at(core::Epoch{w.end().tai_nanoseconds() + 1'000'000}).visible);
            }
        }
        EXPECT_EQ(actual.provenance().target_id, target.id());
        EXPECT_EQ(actual.provenance().data_sha256, s.data.data_lock().sha256());
        EXPECT_EQ(actual.provenance().input_revision, analysis.snapshot().revision);
        EXPECT_NE(actual.provenance().algorithm.find("fixed-grid-bisection-v1"), std::string::npos);
    }
    const auto wide = analysis.access(s.station, s.satellite, {.search_step = 10s});
    const auto narrow = analysis.access(s.station, s.sensor, {.search_step = 10s});
    EXPECT_LT(narrow.windows()[0].duration(), wide.windows()[0].duration());
    scene->document.objects().update(
        s.sensor, app::objects::SensorUpdate{{}, {}, app::objects::Pointing{s.station}});
    const auto tracked =
        runtime::prepare(s.document, s.data).access(s.station, s.sensor, {.search_step = 10s});
    EXPECT_EQ(tracked.windows()[0].end(), wide.windows()[0].end());
}
TEST_F(AnalysisAccess, AllPropagatorsUseTheirNativeFrameAndSelectedTimeData) {
    auto &s = *scene;
    for (const auto model : {app::OrbitPropagator::two_body, app::OrbitPropagator::j2_secular,
                             app::OrbitPropagator::j4_secular, app::OrbitPropagator::sgp4}) {
        SCOPED_TRACE(static_cast<int>(model));
        auto input = circular();
        input.propagator = model;
        if (model == app::OrbitPropagator::sgp4)
            input.tle = {"1 00005U 58002B   00179.78495062  .00000023  00000-0  28098-4 0  4753",
                         "2 00005  34.2682 348.7242 1859667 331.7664  19.3264 10.82419157413667"};
        s.document.objects().update(s.satellite, app::objects::SatelliteUpdate{{}, input, {}});
        const auto analysis = runtime::prepare(s.document, s.data);
        const auto actual = analysis.access(s.station, s.satellite, {30s});
        auto raw = *s.document.snapshot().project;
        raw.earth_orientation = s.data.eop()->data_info();
        runtime::AccessSession legacy{
            {raw, s.data.time(), s.data.frames(), s.data.eop()}, s.station.id(), s.satellite.id()};
        // TLE is deliberately propagated away from its epoch: this checks API/frame routing,
        // not physical prediction accuracy. The independent propagator references are separate.
        const auto expected = core::find_access_intervals(
            raw.first, raw.last, 30'000'000'000, [&](auto t) { return legacy.at(t).visible; });
        ASSERT_EQ(actual.windows().size(), expected.size());
        for (std::size_t i = 0; i < expected.size(); ++i) {
            EXPECT_EQ(actual.windows()[i].start(), expected[i].first);
            EXPECT_EQ(actual.windows()[i].end(), expected[i].last);
        }
        const auto native_frame =
            model == app::OrbitPropagator::sgp4 ? core::FrameId::teme : core::FrameId::gcrf;
        const auto propagation =
            analysis.states(s.satellite, std::span<const core::Epoch>{}, native_frame);
        EXPECT_TRUE(actual.provenance().algorithm.starts_with(propagation.provenance().algorithm));
        EXPECT_EQ(actual.provenance().data_sha256, s.data.data_lock().sha256());
    }
}
TEST_F(AnalysisAccess, SubintervalClippingValidationAndCancellation) {
    const auto &s = *scene;
    const auto analysis = runtime::prepare(s.document, s.data);
    const runtime::AccessOptions options{10s, 1ms, core::Epoch{1}, core::Epoch{10'000'000'003}};
    const auto clipped = analysis.access(s.station, s.sensor, options);
    ASSERT_EQ(clipped.windows().size(), 1U);
    EXPECT_EQ(clipped.windows()[0].start(), *options.first);
    EXPECT_EQ(clipped.windows()[0].end(), *options.last);
    EXPECT_TRUE(clipped.windows()[0].start_clipped());
    EXPECT_TRUE(clipped.windows()[0].end_clipped());
    EXPECT_EQ(clipped.query().search_step, 10s);
    EXPECT_TRUE(analysis.access(s.station, s.sensor, {10s, 1ms, core::Epoch{0}, core::Epoch{0}})
                    .windows()
                    .empty());
    EXPECT_THROW((void)analysis.access(s.station, s.sensor, {0ns}), std::invalid_argument);
    EXPECT_THROW((void)analysis.access(s.station, s.sensor, {1s, 2s}), std::invalid_argument);
    EXPECT_THROW((void)analysis.access(s.station, s.sensor, {1ns, 1ns}), std::invalid_argument);
    EXPECT_THROW(
        (void)analysis.access(s.station, s.sensor, {1s, 1ms, core::Epoch{0}, core::Epoch{-1}}),
        std::out_of_range);
    EXPECT_THROW(
        (void)analysis.access(s.station, s.sensor, {1s, 1ms, core::Epoch{1}, core::Epoch{0}}),
        std::invalid_argument);
    EXPECT_THROW((void)analysis.access(s.satellite, s.station, {10s}), std::invalid_argument);
    EXPECT_THROW((void)runtime::prepare(s.document).access(s.station, s.sensor, {10s}),
                 runtime::AnalysisCapabilityError);
    EXPECT_THROW((void)analysis.access(s.station, s.sensor, {10s}, [] { return true; }),
                 runtime::AnalysisCancelled);
    int calls = 0;
    // Before search, initial sample, next sample, then within boundary bisection.
    EXPECT_THROW((void)analysis.access(s.station, s.sensor, {100s}, [&] { return ++calls == 4; }),
                 runtime::AnalysisCancelled);
    EXPECT_EQ(calls, 4);
    EXPECT_THROW((void)analysis.access(s.station, s.sensor, {10s},
                                       []() -> bool { throw std::logic_error("callback"); }),
                 std::logic_error);
    EXPECT_FALSE(analysis.access(s.station, s.sensor, {10s}).windows().empty());
}
TEST_F(AnalysisAccess, OwnsDeletedObjectsAndTracksOnlyRelevantDependencies) {
    auto &s = *scene;
    const auto analysis = runtime::prepare(s.document, s.data);
    const auto before = analysis.access(s.station, s.sensor, {10s});
    s.document.objects().rename(s.sensor, "Renamed");
    const auto unrelated =
        s.document.objects().create(app::objects::GroundStation{"Other", {1, .5, 0}});
    const auto renamed = runtime::prepare(s.document, s.data).access(s.station, s.sensor, {10s});
    EXPECT_EQ(before.provenance().input_sha256, renamed.provenance().input_sha256);
    EXPECT_EQ(before.provenance().target_name, "Camera");
    EXPECT_EQ(renamed.provenance().target_name, "Renamed");
    s.document.objects().update(s.station, app::objects::GroundStationUpdate{{}, {}, .5});
    const auto changed = runtime::prepare(s.document, s.data).access(s.station, s.sensor, {10s});
    EXPECT_NE(before.provenance().input_sha256, changed.provenance().input_sha256);
    EXPECT_THROW((void)analysis.access(unrelated, s.sensor, {10s}), app::ObjectNotFoundError);
    app::ProjectDocument other{*analysis.snapshot().project};
    EXPECT_THROW((void)analysis.access(other.objects().get(s.station.id()), s.sensor, {10s}),
                 app::ProjectError);
    s.document.objects().remove(s.satellite);
    const auto after = analysis.access(s.station, s.sensor, {10s});
    EXPECT_EQ(before.provenance().input_sha256, after.provenance().input_sha256);
    EXPECT_EQ(before.windows()[0].end(), after.windows()[0].end());
}
TEST_F(AnalysisAccess, PointingTargetLocationIsASeparateDependency) {
    auto &s = *scene;
    const auto target =
        s.document.objects().create(app::objects::GroundStation{"Aim", s.location()});
    s.document.objects().update(s.sensor,
                                app::objects::SensorUpdate{{}, {}, app::objects::Pointing{target}});
    const auto query = [&] {
        return runtime::prepare(s.document, s.data).access(s.station, s.sensor, {10s});
    };
    const auto original = query();
    s.document.objects().update(target, app::objects::GroundStationUpdate{"Name", {}, 1.0});
    EXPECT_EQ(original.provenance().input_sha256, query().provenance().input_sha256);
    s.document.objects().update(
        target, app::objects::GroundStationUpdate{{}, core::GroundStationLocation{0, 1, 0}, {}});
    EXPECT_NE(original.provenance().input_sha256, query().provenance().input_sha256);
    EXPECT_TRUE(query().windows().empty());
}
TEST(AccessResults, DurationUsesExactNanosecondsAndRejectsOverflow) {
    const auto max = std::numeric_limits<std::int64_t>::max();
    const results::AccessWindow window{core::Epoch{max - 13}, core::Epoch{max}, true, true};
    EXPECT_EQ(window.duration(), 13ns);
    EXPECT_THROW((results::AccessWindow{core::Epoch{1}, core::Epoch{1}, false, false}),
                 std::invalid_argument);
    EXPECT_THROW((results::AccessWindow{core::Epoch{-1}, core::Epoch{max}, false, false}),
                 std::invalid_argument);
    EXPECT_THROW((results::AccessWindows{{window}, {core::Epoch{0}, core::Epoch{1}, 1s, 1ms}, {}}),
                 std::invalid_argument);
}
TEST_F(AnalysisAccess, AerMatchesItrfGeometryAndRangeDerivative) {
    const auto &s = *scene;
    const auto analysis = runtime::prepare(s.document, s.data);
    const std::array times{core::Epoch{100'000'000'003}, core::Epoch{0},
                           core::Epoch{100'000'000'003}};
    const auto result = analysis.aer(s.station, s.satellite, times);
    ASSERT_EQ(result.samples().size(), times.size());
    for (std::size_t i = 0; i < times.size(); ++i) {
        const auto &sample = result.samples()[i];
        const auto reference = core::observe_ground_target(
            s.location(), analysis.state(s.satellite, times[i], core::FrameId::itrf));
        EXPECT_EQ(sample.epoch, times[i]);
        EXPECT_EQ(sample.observation.azimuth_rad, reference.azimuth_rad);
        EXPECT_DOUBLE_EQ(sample.observation.elevation_rad, reference.elevation_rad);
        EXPECT_DOUBLE_EQ(sample.observation.range_m, reference.range_m);
        EXPECT_DOUBLE_EQ(sample.observation.range_rate_mps, reference.range_rate_mps);
        EXPECT_EQ(sample.observation.line_of_sight, reference.line_of_sight);
        EXPECT_FALSE(sample.off_boresight_rad.has_value());
        EXPECT_EQ(sample.visible, reference.line_of_sight && reference.elevation_rad >= 0);
    }
    const std::array derivative_times{core::Epoch{999'990'000'000}, core::Epoch{1'000'000'000'000},
                                      core::Epoch{1'000'010'000'000}};
    const auto derivative = analysis.aer(s.station, s.satellite, derivative_times);
    const auto &values = derivative.samples();
    EXPECT_NEAR(values[1].observation.range_rate_mps,
                (values[2].observation.range_m - values[0].observation.range_m) / .02, .01);
    const auto access = analysis.access(s.station, s.satellite, {10s});
    EXPECT_EQ(result.provenance().input_sha256, access.provenance().input_sha256);
    EXPECT_EQ(result.provenance().data_sha256, access.provenance().data_sha256);
    EXPECT_EQ(result.provenance().input_revision, analysis.snapshot().revision);
}
TEST_F(AnalysisAccess, AerSamplingBoundsAndCancellation) {
    const auto &s = *scene;
    const auto analysis = runtime::prepare(s.document, s.data);
    const auto sampled =
        analysis.aer(s.station, s.sensor, 10s, core::Epoch{3}, core::Epoch{10'000'000'007});
    ASSERT_EQ(sampled.samples().size(), 3);
    EXPECT_EQ(sampled.samples()[0].epoch, core::Epoch{3});
    EXPECT_EQ(sampled.samples()[1].epoch, core::Epoch{10'000'000'003});
    EXPECT_EQ(sampled.samples()[2].epoch, core::Epoch{10'000'000'007});
    EXPECT_TRUE(sampled.samples()[0].off_boresight_rad.has_value());
    EXPECT_EQ(
        analysis.aer(s.station, s.sensor, 1s, core::Epoch{0}, core::Epoch{0}).samples().size(), 1);
    const auto empty = analysis.aer(s.station, s.sensor, std::span<const core::Epoch>{});
    EXPECT_TRUE(empty.samples().empty());
    EXPECT_EQ(empty.provenance().input_sha256, sampled.provenance().input_sha256);
    EXPECT_THROW((void)analysis.aer(s.station, s.sensor, 0ns), std::invalid_argument);
    EXPECT_THROW((void)analysis.aer(s.station, s.sensor, 1ns), std::invalid_argument);
    EXPECT_THROW((void)analysis.aer(s.station, s.sensor, std::array{core::Epoch{-1}}),
                 std::out_of_range);
    EXPECT_THROW((void)analysis.aer(s.satellite, s.sensor, 10s), std::invalid_argument);
    EXPECT_THROW((void)runtime::prepare(s.document).aer(s.station, s.sensor, 10s),
                 runtime::AnalysisCapabilityError);
    std::size_t checks = 0;
    EXPECT_THROW(
        (void)analysis.aer(s.station, s.sensor, 10s, {}, {}, [&] { return ++checks == 5; }),
        runtime::AnalysisCancelled);
    EXPECT_EQ(checks, 5);
}
TEST_F(AnalysisAccess, AllReportTemplatesMatchExistingEngine) {
    const auto &s = *scene;
    const auto analysis = runtime::prepare(s.document, s.data);
    auto p = *analysis.snapshot().project;
    p.earth_orientation = s.data.eop()->data_info();
    const runtime::ReportContext context{p, s.data.time(), s.data.frames(), s.data.eop()};
    for (const auto kind : {app::ReportKind::scenario_summary, app::ReportKind::object_summary,
                            app::ReportKind::cartesian_state, app::ReportKind::ground_stations,
                            app::ReportKind::classical_elements, app::ReportKind::geodetic_position,
                            app::ReportKind::orbital_motion, app::ReportKind::access_intervals,
                            app::ReportKind::aer}) {
        SCOPED_TRACE(static_cast<int>(kind));
        app::ReportDefinition definition;
        definition.id = "query";
        definition.name = "Report";
        definition.kind = kind;
        definition.step_nanoseconds = 10'000'000'000;
        if (app::report_is_access(kind))
            definition.object_ids = {s.station.id(), s.sensor.id()};
        const auto expected = runtime::generate_report(context, definition);
        const auto actual = analysis.report(definition);
        EXPECT_EQ(actual.table.rows(), expected.table.rows());
        ASSERT_EQ(actual.table.columns().size(), expected.table.columns().size());
        for (std::size_t c = 0; c < actual.table.columns().size(); ++c) {
            EXPECT_EQ(actual.table.columns()[c].id, expected.table.columns()[c].id);
            EXPECT_EQ(actual.column_title(c), expected.column_title(c));
        }
        EXPECT_EQ(actual.input_sha256, expected.input_sha256);
        EXPECT_EQ(actual.data_sha256, s.data.data_lock().sha256());
        EXPECT_EQ(actual.input_revision, analysis.snapshot().revision);
        if (kind == app::ReportKind::aer) {
            const auto aer = analysis.aer(s.station, s.sensor, 10s);
            ASSERT_EQ(actual.table.rows().size(), aer.samples().size());
            for (std::size_t i = 0; i < aer.samples().size(); ++i) {
                EXPECT_DOUBLE_EQ(std::get<double>(actual.table.rows()[i][5]),
                                 aer.samples()[i].observation.range_m);
                EXPECT_EQ(std::get<std::int64_t>(actual.table.rows()[i][7]),
                          aer.samples()[i].visible);
            }
        }
    }
}
TEST_F(AnalysisAccess, ReportsAndAerKeepDeletedObjectsAndCaptureBeforeCallbacks) {
    auto &s = *scene;
    const auto analysis = runtime::prepare(s.document, s.data);
    app::ReportDefinition definition;
    definition.id = "query";
    definition.name = "Original";
    definition.kind = app::ReportKind::aer;
    definition.object_ids = {s.station.id(), s.sensor.id()};
    const auto report = analysis.report(definition);
    const auto aer = analysis.aer(s.station, s.sensor, 60s);
    const auto modified = analysis.report(definition, {}, [&](auto, auto) {
        definition.name = "Changed";
        definition.object_ids.clear();
    });
    EXPECT_EQ(modified.definition.name, "Original");
    EXPECT_EQ(modified.table.rows(), report.table.rows());
    s.document.objects().remove(s.satellite);
    const auto repeated = analysis.report(report.definition);
    EXPECT_EQ(repeated.table.rows(), report.table.rows());
    const auto repeat_aer = analysis.aer(s.station, s.sensor, 60s);
    ASSERT_EQ(repeat_aer.samples().size(), aer.samples().size());
    EXPECT_EQ(repeat_aer.samples().back().observation.range_m,
              aer.samples().back().observation.range_m);
    // Cancellation at the completion callback must still prevent result publication.
    bool cancel = false;
    EXPECT_THROW((void)analysis.report(
                     report.definition, [&] { return cancel; },
                     [&](auto current, auto total) { cancel = current == total; }),
                 runtime::AnalysisCancelled);
}
TEST_F(AnalysisAccess, ReportsNeedOnlyTheirActualCapabilities) {
    const auto &s = *scene;
    const auto analysis = runtime::prepare(s.document);
    app::ReportDefinition d;
    d.id = "native";
    d.name = "Native state";
    d.object_ids = {s.satellite.id()};
    EXPECT_THROW((void)analysis.report(d), runtime::AnalysisCapabilityError); // UTC is explicit.
    d.time_scale = app::ReportTimeScale::tai;
    EXPECT_NO_THROW((void)analysis.report(d));
    d.frame = core::FrameId::itrf;
    EXPECT_THROW((void)analysis.report(d), runtime::AnalysisCapabilityError);
    d.kind = app::ReportKind::ground_stations;
    d.object_ids = {s.station.id()};
    EXPECT_NO_THROW((void)analysis.report(d));
}
