#include <fstream>
#include <gtest/gtest.h>
#include <orbitforge/runtime/analysis.hpp>
#include <orbitforge/runtime/time_frames.hpp>

using namespace orbitforge;
using namespace orbitforge::application;
namespace {
class ProjectPersistence : public ::testing::Test {
  protected:
    std::filesystem::path folder;
    void SetUp() override {
        folder = std::filesystem::path(ORBITFORGE_INTEGRATION_WORKSPACE).parent_path() /
                 ("project-analysis-" +
                  std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
        std::filesystem::create_directory(folder);
    }
    void TearDown() override {
        if (folder.is_absolute() &&
            folder.parent_path() ==
                std::filesystem::path(ORBITFORGE_INTEGRATION_WORKSPACE).parent_path() &&
            folder.filename().string().starts_with("project-analysis-")) {
            std::error_code ignored;
            std::filesystem::remove_all(folder, ignored);
        }
    }
    ProjectDocument make_project() {
        Project value{"Persist", core::Epoch{0}, core::Epoch{100}};
        value.ground_stations.push_back({"station", "Station", {0, 0, 0}});
        return ProjectDocument{std::move(value)};
    }
};
TEST_F(ProjectPersistence, SavedAnalysisOwnsItsProjectAndIgnoresPresentationOnlyChanges) {
    auto document = make_project();
    const auto analysis = runtime::prepare(document);
    const auto station = document.objects().get("station");
    document.objects().rename(station, "Renamed");
    runtime::save_project(document, folder / "current.orbitforge", analysis);
    EXPECT_FALSE(document.dirty());
    const auto loaded = application::load_project(folder / "current.orbitforge");
    EXPECT_EQ(loaded.ground_stations[0].name(), "Renamed");
    EXPECT_EQ(loaded.analysis_status(), AnalysisStatus::current);
    EXPECT_EQ(loaded.analysis_record->data_sha256, DataLock{}.sha256());
    EXPECT_EQ(encode_project(*document.snapshot().project), encode_project(loaded));
    auto other = ProjectDocument{loaded};
    EXPECT_THROW(runtime::save_project(other, folder / "other.orbitforge", analysis), ProjectError);
    objects::GroundStationUpdate change;
    change.minimum_elevation_rad = 0.1;
    document.objects().update(station, change);
    EXPECT_EQ(document.snapshot().project->analysis_status(), AnalysisStatus::stale);
    EXPECT_THROW(runtime::save_project(document, folder / "stale.orbitforge", analysis),
                 RevisionConflictError);
    EXPECT_FALSE(std::filesystem::exists(folder / "stale.orbitforge"));
    document.save(folder / "inputs.orbitforge");
    EXPECT_EQ(application::load_project(folder / "inputs.orbitforge").analysis_status(),
              AnalysisStatus::stale);
}
TEST_F(ProjectPersistence, FailureAndRevisionConflictDoNotPublishOrNotify) {
    auto document = make_project();
    const auto analysis = runtime::prepare(document);
    const auto path = folder / "existing.orbitforge";
    document.save(path);
    const auto before = document.snapshot();
    int events = 0;
    const auto token = document.subscribe([&](const ChangeSet &) { ++events; });
    EXPECT_THROW(runtime::save_project(document, path, analysis), ProjectError);
    EXPECT_THROW(runtime::save_project(document, folder / "absent" / "file", analysis),
                 std::filesystem::filesystem_error);
    EXPECT_EQ(document.snapshot().revision, before.revision);
    EXPECT_FALSE(document.snapshot().project->analysis_record);
    EXPECT_EQ(events, 0);
    EXPECT_EQ(encode_project(application::load_project(path)), encode_project(*before.project));
    document.objects().rename(document.objects().get("station"), "New");
    EXPECT_THROW(document.save(path, before, true), RevisionConflictError);
    EXPECT_EQ(encode_project(application::load_project(path)), encode_project(*before.project));
    document.unsubscribe(token);
}
TEST_F(ProjectPersistence, SaveObserversSeePublishedFileAndReentrantEditsRemainDirty) {
    auto document = make_project();
    const auto analysis = runtime::prepare(document);
    const auto path = folder / "observed.orbitforge";
    int events = 0;
    const auto token = document.subscribe([&](const ChangeSet &event) {
        if (++events != 1)
            return;
        EXPECT_FALSE(document.dirty());
        EXPECT_TRUE(event.settings_changed);
        EXPECT_EQ(encode_project(application::load_project(path)),
                  encode_project(*event.after.project));
        document.objects().rename(document.objects().get("station"), "After save");
    });
    runtime::save_project(document, path, analysis);
    EXPECT_EQ(events, 2);
    EXPECT_TRUE(document.dirty());
    EXPECT_EQ(application::load_project(path).ground_stations[0].name(), "Station");
    document.unsubscribe(token);
}
TEST_F(ProjectPersistence, SavedCandidateCannotBeMutatedThroughTheCallersAlias) {
    auto document = make_project();
    auto candidate = document.snapshot();
    auto alias = std::make_shared<Project>(*candidate.project);
    alias->name = "Saved candidate";
    candidate.project = alias;
    document.save(folder / "candidate.orbitforge", candidate);
    alias->name = "Mutated after publication";
    EXPECT_EQ(document.snapshot().project->name, "Saved candidate");
    EXPECT_FALSE(document.dirty());
    EXPECT_EQ(application::load_project(folder / "candidate.orbitforge").name, "Saved candidate");
}
TEST_F(ProjectPersistence, TimeOnlyRoundTripAndOpenConsentKeepExactPlan) {
    if (!runtime::time_frames_available())
        GTEST_SKIP();
    auto document = make_project();
    const auto data = runtime::DataSnapshot::builtin_time();
    const auto analysis = runtime::prepare(document, data);
    const auto path = folder / "utc.orbitforge";
    runtime::save_project(document, path, analysis);
    auto stored = application::load_project(path);
    ASSERT_TRUE(stored.time_data);
    EXPECT_FALSE(stored.earth_orientation);
    EXPECT_EQ(stored.data_lock(), data.data_lock());
    EXPECT_EQ(stored.analysis_status(), AnalysisStatus::current);
    EXPECT_THROW((void)runtime::open_project(path), runtime::AnalysisCapabilityError);
    EXPECT_FALSE(runtime::open_project(path, data).dirty());
    stored.time_data->id += "-previous";
    application::save_project(path, stored, true);
    try {
        (void)runtime::open_project(path, data);
        FAIL() << "Expected explicit consent";
    } catch (const runtime::OpenDataChangedError &error) {
        ASSERT_EQ(error.plan().data_changes().size(), 1U);
        EXPECT_EQ(error.plan().data_changes()[0].role, "utc-time");
        EXPECT_THROW((void)error.plan().open(), runtime::OpenDataChangedError);
        // Neither the source file nor the owner's lifetime is needed after planning.
        std::filesystem::remove(path);
        auto opened = error.plan().open(true);
        EXPECT_EQ(opened.snapshot().project->data_lock(), data.data_lock());
        EXPECT_TRUE(opened.dirty());
        opened.objects().rename(opened.objects().get("station"), "First opening");
        auto again = error.plan().open(true);
        EXPECT_EQ(again.snapshot().project->ground_stations[0].name(), "Station");
    }
}
TEST_F(ProjectPersistence, FingerprintCoversPhysicalInputsButNotAppearanceReportsOrOrdering) {
    auto original = *make_project().snapshot().project;
    ProjectSatellite satellite;
    satellite.orbit.last = original.last;
    original.satellites.push_back(satellite);
    original.sensors.push_back({"sensor", "Sensor", satellite.orbit.object_id});
    const auto hash = analysis_input_sha256(original);
    auto changed = original;
    changed.name = "Title";
    changed.current = core::Epoch{1};
    changed.satellites[0].display_name = "New name";
    changed.satellites[0].display.line_width = 3;
    changed.satellites[0].model_path = "unloaded.glb";
    changed.satellites[0].orbit.step_seconds = 5;
    changed.satellites[0].orbit.input_frame = core::FrameId::itrf;
    EXPECT_EQ(analysis_input_sha256(changed), hash);
    changed.sensors[0].half_angle_rad = 0.1;
    EXPECT_NE(analysis_input_sha256(changed), hash);
    changed = original;
    changed.satellites[0].orbit.initial.velocity_mps[1] += 1;
    EXPECT_NE(analysis_input_sha256(changed), hash);
    changed = original;
    changed.last = core::Epoch{101};
    EXPECT_NE(analysis_input_sha256(changed), hash);
}
} // namespace
