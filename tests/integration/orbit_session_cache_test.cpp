#include <future>
#include <gtest/gtest.h>
#include <orbitforge/runtime/analysis.hpp>
#include <orbitforge/runtime/project.hpp>
#include <orbitforge/runtime/time_frames.hpp>
#include <orbitforge/runtime/two_body.hpp>

using namespace orbitforge;
using namespace std::chrono_literals;
namespace {
application::Project numerical_project() {
    application::Project project{"Shared science", core::Epoch{0}, core::Epoch{1800'000'000'000}};
    application::ProjectSatellite satellite;
    satellite.orbit.propagator = application::OrbitPropagator::hpop;
    satellite.orbit.first = project.first;
    satellite.orbit.last = project.last;
    satellite.orbit.hpop.gravity_degree = satellite.orbit.hpop.gravity_order = 0;
    project.satellites.push_back(satellite);
    return project;
}
auto sampled(const application::Session &session, core::Epoch at, int &checkpoints) {
    return session.sample(std::array{at}, [&] { ++checkpoints; }).states().front();
}
class ChangedEop final : public core::IEarthOrientationProvider {
  public:
    explicit ChangedEop(std::shared_ptr<const core::IEarthOrientationProvider> original)
        : original_(std::move(original)), info_(original_->data_info()) {
        info_.sha256 = std::string(64, 'a');
    }
    core::EarthOrientationState at(core::Epoch epoch) const override {
        return original_->at(epoch);
    }
    const core::EopDataInfo &data_info() const noexcept override { return info_; }

  private:
    std::shared_ptr<const core::IEarthOrientationProvider> original_;
    core::EopDataInfo info_;
};
} // namespace

TEST(OrbitSessionCache, FirstReportUsesPreparedScienceWithItsOwnRevisionAndSampling) {
    const auto input = numerical_project();
    const auto &orbit = input.satellites.front().orbit;
    auto cache = std::make_shared<runtime::OrbitSessionCache>();
    const auto displayed = cache->acquire(orbit, 17);
    int cold_checks = 0;
    const auto expected = sampled(displayed, input.last, cold_checks);
    ASSERT_GT(cold_checks, 0);
    const application::ProjectDocument document{input};
    const auto analysis = runtime::prepare(document, {}, cache);
    application::ReportQuery query;
    query.kind = application::ReportKind::cartesian_state;
    query.object_ids = {orbit.object_id};
    query.step_nanoseconds = 900'000'000'000;
    application::ReportTemplate format;
    format.time_scale = application::ReportTimeScale::tai;
    int warm_checks = 0;
    const auto report = analysis.report(query, format, [&] {
        ++warm_checks;
        return false;
    });
    int independent_checks = 0;
    const auto independent = runtime::prepare(document).report(query, format, [&] {
        ++independent_checks;
        return false;
    });
    EXPECT_EQ(report.table.rows(), independent.table.rows());
    ASSERT_EQ(report.table.rows().size(), 3U);
    const auto trajectory = analysis.states(document.objects().get(orbit.object_id),
                                            std::array{input.last}, core::FrameId::gcrf);
    EXPECT_EQ(trajectory.states().front().position_m, expected.position_m);
    EXPECT_EQ(trajectory.states().front().velocity_mps, expected.velocity_mps);
    EXPECT_EQ(trajectory.provenance().input_revision, document.snapshot().revision);
    EXPECT_EQ(displayed.provenance().input_revision, 17U);
    EXPECT_EQ(report.input_revision, document.snapshot().revision);
    int cached_checks = 0;
    (void)sampled(cache->acquire(orbit, 28), input.last, cached_checks);
    EXPECT_LT(cached_checks * 5, cold_checks);
    // Table assembly and provenance also use cancellation checkpoints. Only the
    // scientific integration work should disappear from the first shared report.
    EXPECT_EQ(independent_checks - warm_checks, cold_checks - cached_checks);
}

TEST(OrbitSessionCache, RenamingAllObjectKindsPreservesHpopAndReferences) {
    auto input = numerical_project();
    input.ground_stations.push_back({"station", "Station", {}});
    application::ProjectSensor sensor;
    sensor.object_id = "sensor";
    sensor.parent_satellite_id = input.satellites[0].orbit.object_id;
    sensor.pointing = application::SensorPointing::targeted;
    sensor.target_station_id = "station";
    input.sensors.push_back(sensor);
    application::ReportDefinition definition;
    definition.id = "report";
    definition.name = "Report";
    definition.query.object_ids = {sensor.parent_satellite_id};
    input.reports.push_back(definition);
    application::ProjectDocument document{input};
    const auto cache = std::make_shared<runtime::OrbitSessionCache>();
    const auto &orbit = input.satellites[0].orbit;
    int cold = 0, warm = 0, renamed_checks = 0;
    const auto expected = sampled(cache->acquire(orbit, 1), input.last, cold);
    (void)sampled(cache->acquire(orbit, 1), input.last, warm);
    const auto hash = runtime::report_input_sha256(input);
    for (const auto &ref : document.objects().find())
        document.objects().rename(ref, "Renamed " + ref.id());
    const auto &updated = *document.snapshot().project;
    EXPECT_EQ(updated.satellites[0].orbit.object_id, orbit.object_id);
    EXPECT_EQ(updated.ground_stations[0].object_id, "station");
    EXPECT_EQ(updated.sensors[0].object_id, "sensor");
    EXPECT_EQ(updated.sensors[0].parent_satellite_id, orbit.object_id);
    EXPECT_EQ(updated.sensors[0].target_station_id, "station");
    EXPECT_EQ(updated.reports[0].query.object_ids, definition.query.object_ids);
    EXPECT_EQ(runtime::report_input_sha256(updated), hash);
    const auto actual =
        sampled(cache->acquire(updated.satellites[0].orbit, 1), input.last, renamed_checks);
    EXPECT_EQ(actual.position_m, expected.position_m);
    EXPECT_EQ(actual.velocity_mps, expected.velocity_mps);
    EXPECT_EQ(renamed_checks, warm);
    EXPECT_GT(cold, warm);
    const auto restored = application::decode_project(application::encode_project(updated));
    EXPECT_EQ(restored.satellites[0].name(), updated.satellites[0].name());
    EXPECT_EQ(restored.ground_stations[0].name(), updated.ground_stations[0].name());
    EXPECT_EQ(restored.sensors[0].name(), updated.sensors[0].name());
    EXPECT_EQ(runtime::report_input_sha256(restored), hash);
}

TEST(OrbitSessionCache, ForkCancellationAndPruningPreservePublishedSolutions) {
    const auto input = numerical_project();
    const auto &orbit = input.satellites.front().orbit;
    runtime::OrbitSessionCache published;
    const auto original = published.acquire(orbit, 1);
    int cold = 0;
    const auto expected = sampled(original, input.last, cold);
    auto candidate = published.fork();
    auto edited = orbit;
    edited.initial.velocity_mps[1] += 10;
    const auto updating = candidate->acquire(edited, 2);
    int checks = 0;
    EXPECT_THROW((void)updating.sample(std::array{input.last},
                                       [&] {
                                           if (++checks == 10)
                                               throw std::runtime_error("cancel");
                                       }),
                 std::runtime_error);
    int warm = 0;
    EXPECT_EQ(sampled(published.acquire(orbit, 1), input.last, warm).position_m,
              expected.position_m);
    EXPECT_LT(warm * 5, cold);
    int resumed = 0, reference = 0;
    const auto actual = sampled(updating, input.last, resumed);
    const auto fresh = sampled(runtime::make_orbit_session(edited), input.last, reference);
    EXPECT_EQ(actual.position_m, fresh.position_m);
    EXPECT_EQ(actual.velocity_mps, fresh.velocity_mps);
    EXPECT_EQ(resumed, reference); // Cancellation never publishes a partial ephemeris.
    candidate->retain_objects({});
    int after_pruning = 0;
    (void)sampled(candidate->acquire(edited, 3), input.last, after_pruning);
    EXPECT_EQ(after_pruning, reference);
    warm = 0;
    (void)sampled(original, input.last, warm);
    EXPECT_LT(warm * 5, cold); // Eviction does not destroy existing readers.
}

TEST(OrbitSessionCache, ActiveInputsInvalidateButDisplayAndInactiveDraftsReuse) {
    auto orbit = numerical_project().satellites.front().orbit;
    runtime::OrbitSessionCache cache;
    int cold = 0;
    const auto initial = sampled(cache.acquire(orbit, 1), orbit.last, cold);
    orbit.step_seconds = 5; // Display sampling is independent of adaptive scientific output.
    orbit.input_frame = core::FrameId::itrf;
    orbit.hpop.fixed_step_seconds += 1; // Inactive while the integrator is adaptive.
    int warm = 0;
    EXPECT_EQ(sampled(cache.acquire(orbit, 2), orbit.last, warm).position_m, initial.position_m);
    EXPECT_LT(warm * 5, cold);
    orbit.hpop.max_step_seconds /= 2;
    int updated = 0, reference = 0;
    (void)sampled(cache.acquire(orbit, 3), orbit.last, updated);
    (void)sampled(runtime::make_orbit_session(orbit), orbit.last, reference);
    EXPECT_EQ(updated, reference);
    EXPECT_GT(updated, warm * 5);
    orbit.hpop.report_ephemeris_on_fixed_step = true;
    updated = 0;
    (void)sampled(cache.acquire(orbit, 4), orbit.last, updated);
    reference = 0;
    (void)sampled(runtime::make_orbit_session(orbit), orbit.last, reference);
    EXPECT_EQ(updated, reference);
    EXPECT_GT(updated, warm * 5);
    orbit.step_seconds = 6;
    updated = 0;
    (void)sampled(cache.acquire(orbit, 5), orbit.last, updated);
    reference = 0;
    (void)sampled(runtime::make_orbit_session(orbit), orbit.last, reference);
    EXPECT_EQ(updated, reference);
    EXPECT_GT(updated, warm * 5);
}

TEST(OrbitSessionCache, ChangedResourcesAndFramesRecomputeAndMissingResourcesAreRejected) {
    if (!runtime::time_frames_available())
        GTEST_SKIP();
    auto orbit = numerical_project().satellites.front().orbit;
    orbit.hpop.gravity_degree = orbit.hpop.gravity_order = 2;
    const auto data = runtime::DataSnapshot::load(ORBITFORGE_REPORT_EOP);
    auto environment = data.force_environment(orbit);
    runtime::OrbitSessionCache cache;
    int cold = 0;
    const auto expected =
        sampled(cache.acquire(orbit, 1, data.time().get(), environment), orbit.last, cold);
    ASSERT_GT(cold, 0);
    int warm = 0;
    (void)sampled(cache.acquire(orbit, 2, data.time().get(), environment), orbit.last, warm);
    EXPECT_LT(warm * 5, cold);
    environment.eop = std::make_shared<ChangedEop>(data.eop());
    int changed = 0;
    EXPECT_EQ(sampled(cache.acquire(orbit, 3, data.time().get(), environment), orbit.last, changed)
                  .position_m,
              expected.position_m);
    EXPECT_EQ(changed, cold);
    environment.frames = runtime::make_frame_provider();
    changed = 0;
    (void)sampled(cache.acquire(orbit, 4, data.time().get(), environment), orbit.last, changed);
    EXPECT_EQ(changed, cold);
    auto missing = environment;
    missing.frames.reset();
    EXPECT_THROW((void)cache.acquire(orbit, 5, data.time().get(), missing), std::invalid_argument);
    warm = 0;
    (void)sampled(cache.acquire(orbit, 6, data.time().get(), environment), orbit.last, warm);
    EXPECT_LT(warm * 5, cold);
    auto project = numerical_project();
    project.satellites.front().orbit = orbit;
    project.earth_orientation = data.eop()->data_info();
    const application::ProjectDocument document{project};
    const auto changed_data =
        runtime::DataSnapshot::from_providers(data.time(), environment.frames, environment.eop);
    runtime::AnalysisPlan plan{document, changed_data, cache.fork()};
    EXPECT_FALSE(plan.data_changes().empty());
    EXPECT_THROW((void)plan.prepare(), runtime::DataChangedError);
    EXPECT_NO_THROW((void)plan.prepare(true));
}

TEST(OrbitSessionCache, SharedConcurrentReadersKeepExactResultsAndIndependentRevisions) {
    auto orbit = numerical_project().satellites.front().orbit;
    auto cache = std::make_shared<runtime::OrbitSessionCache>();
    int cold = 0;
    const auto expected = sampled(cache->acquire(orbit, 1), orbit.last, cold);
    std::vector<std::future<results::StateSeries>> jobs;
    for (std::uint64_t revision = 2; revision < 10; ++revision)
        jobs.push_back(std::async(std::launch::async, [cache, orbit, revision] {
            return cache->acquire(orbit, revision).sample(std::array{orbit.last});
        }));
    for (std::size_t i = 0; i < jobs.size(); ++i) {
        const auto result = jobs[i].get();
        EXPECT_EQ(result.provenance().input_revision, i + 2);
        EXPECT_EQ(result.states().front().position_m, expected.position_m);
        EXPECT_EQ(result.states().front().velocity_mps, expected.velocity_mps);
    }
}

TEST(OrbitSessionCache, CachedSgp4CannotBorrowMissingUtcFromEopMetadata) {
    if (!runtime::time_frames_available())
        GTEST_SKIP();
    const auto data = runtime::DataSnapshot::load(ORBITFORGE_REPORT_EOP);
    application::OrbitInput orbit;
    orbit.propagator = application::OrbitPropagator::sgp4;
    orbit.tle = {"1 00005U 58002B   00179.78495062  .00000023  00000-0  28098-4 0  4753",
                 "2 00005  34.2682 348.7242 1859667 331.7664  19.3264 10.82419157413667"};
    auto environment = data.force_environment(orbit);
    runtime::OrbitSessionCache cache;
    const auto session = cache.acquire(orbit, 1, data.time().get(), environment);
    // EOP provenance contains its UTC dependency, but does not substitute for the
    // actual clock required by SGP4. A cache hit must not bypass that requirement.
    EXPECT_THROW((void)cache.acquire(orbit, 2, nullptr, environment), std::invalid_argument);
    const auto epoch = data.time()->from_utc(core::parse_tle(orbit.tle).epoch_utc);
    EXPECT_EQ(cache.acquire(orbit, 3, data.time().get(), environment)
                  .sample(std::array{epoch})
                  .states()
                  .front()
                  .position_m,
              session.sample(std::array{epoch}).states().front().position_m);
}

TEST(OrbitSessionCache, RenamedMissionSegmentsNeverReturnPreviousEventLabels) {
    if (!runtime::two_body_available())
        GTEST_SKIP();
    application::OrbitInput input;
    input.propagator = application::OrbitPropagator::mission;
    input.last = core::Epoch{120'000'000'000};
    input.mission = {core::MissionCoastModel::two_body,
                     {core::MissionSegment::coast(60s),
                      core::MissionSegment::impulse({0, 10, 0}, core::ManeuverFrame::gcrf, "First"),
                      core::MissionSegment::coast(60s)}};
    runtime::OrbitSessionCache cache;
    const auto before = cache.acquire(input, 1);
    const auto key = runtime::orbit_calculation_key(input);
    input.mission.segments[1].name = "Renamed";
    EXPECT_NE(runtime::orbit_calculation_key(input), key);
    const auto after = cache.acquire(input, 2);
    EXPECT_EQ(before.maneuvers(input.first, input.last).front().name, "First");
    EXPECT_EQ(after.maneuvers(input.first, input.last).front().name, "Renamed");
    EXPECT_EQ(before.sample(std::array{input.last}).states().front().position_m,
              after.sample(std::array{input.last}).states().front().position_m);
}
