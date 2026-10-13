#include <gtest/gtest.h>
#include <orbitforge/orbitforge.hpp>

namespace of = orbitforge;
using namespace std::chrono_literals;

namespace {
class ProjectFacade : public ::testing::Test {
  protected:
    std::filesystem::path folder;
    void SetUp() override {
        folder = std::filesystem::path(ORBITFORGE_INTEGRATION_WORKSPACE).parent_path() /
                 ("project-facade-" +
                  std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
        std::filesystem::create_directory(folder);
    }
    void TearDown() override {
        if (folder.is_absolute() &&
            folder.parent_path() ==
                std::filesystem::path(ORBITFORGE_INTEGRATION_WORKSPACE).parent_path() &&
            folder.filename().string().starts_with("project-facade-")) {
            std::error_code ignored;
            std::filesystem::remove_all(folder, ignored);
        }
    }
};

TEST_F(ProjectFacade, TransactionAnalysisAndReportsShareOneDocument) {
    if (!ORBITFORGE_EXPECT_SPICE) {
        GTEST_SKIP();
    }
    of::Project project{"Observation", of::Epoch{0}, of::Epoch{120'000'000'000}};
    std::vector<of::ChangeSet> events;
    const auto token =
        project.subscribe([&](const of::ChangeSet &event) { events.push_back(event); });
    auto edit = project.edit();
    const auto satellite = edit.objects().create(of::objects::Satellite{"Observer", {}, {}});
    const auto station = edit.objects().create(of::objects::GroundStation{"Station", {}, 0});
    const auto sensor =
        edit.objects().create(of::objects::Sensor{"Camera", satellite, 0.2, {station}});
    EXPECT_TRUE(project.objects().find().empty());
    edit.commit();
    ASSERT_EQ(events.size(), 1U);
    EXPECT_EQ(events[0].added.size(), 3U);
    EXPECT_EQ(project.objects().children(satellite), std::vector{sensor});

    const auto analysis = project.prepare();
    const auto series = analysis.states(satellite, 45s, of::FrameId::gcrf);
    ASSERT_EQ(series.states().size(), 4U);
    EXPECT_EQ(series.states().back().epoch, project.last());
    EXPECT_THROW((void)analysis.state(sensor, project.first(), of::FrameId::gcrf),
                 of::AnalysisCapabilityError);
    const of::ReportQuery query{.kind = of::ReportKind::cartesian_state,
                                .object_ids = {satellite.id()}};
    const of::ReportTemplate format{.columns = {{"state.x", "km", "Position X"}},
                                    .time_scale = of::ReportTimeScale::tai};
    project.set_reports({{"orbit", "Orbit", query, format}});
    const auto result = analysis.report(query, format);
    ASSERT_EQ(result.table.columns().size(), 1U);
    EXPECT_EQ(result.column_title(0), "Position X (km)");
    EXPECT_DOUBLE_EQ(*result.numeric_value(0, 0), 7000);
    EXPECT_EQ(result.input_revision, analysis.snapshot().revision);

    project.objects().rename(satellite, "Renamed");
    EXPECT_EQ(
        std::get<of::application::ProjectSatellite>(analysis.snapshot().read(satellite)).name(),
        "Observer");
    project.save(folder / "observation.orbitforge", analysis);
    EXPECT_FALSE(project.dirty());
    EXPECT_EQ(project.analysis_status(), of::AnalysisStatus::current);
    auto reopened = of::Project::open(folder / "observation.orbitforge");
    EXPECT_EQ(reopened.name(), "Observation");
    EXPECT_EQ(reopened.snapshot().project->reports[0].format, format);
    EXPECT_EQ(reopened.objects().get(sensor.id()).kind(), of::ObjectKind::sensor);
    project.unsubscribe(token);
}

TEST_F(ProjectFacade, CopiesAndDocumentBridgeShareIdentityButDataCopiesDoNot) {
    of::Project original{"Original", of::Epoch{0}, of::Epoch{100}};
    const auto station = original.objects().create(of::objects::GroundStation{"Station", {}, 0});
    auto copy = original;
    auto bridged = of::Project::from_document(original.document());
    int events = 0;
    const auto token = original.subscribe([&](const of::ChangeSet &) { ++events; });
    copy.update({.name = "Copy edits original", .current = of::Epoch{50}});
    bridged.objects().rename(station, "Shared station");
    EXPECT_EQ(original.name(), "Copy edits original");
    EXPECT_EQ(original.current(), of::Epoch{50});
    EXPECT_EQ(original.revision(), bridged.revision());
    EXPECT_EQ(events, 2);
    auto detached_data = original.to_data();
    detached_data.name = "Detached";
    auto detached = of::Project::from_data(detached_data);
    EXPECT_EQ(original.name(), "Copy edits original");
    EXPECT_EQ(detached.name(), "Detached");
    EXPECT_THROW((void)detached.objects().read(station), of::ProjectError);
    EXPECT_THROW((void)detached.prepare().state(station, of::Epoch{0}, of::FrameId::itrf),
                 of::ProjectError);
    original.unsubscribe(token);
}

TEST_F(ProjectFacade, FailedEditsAndUncommittedDraftsDoNotChangePublishedInputs) {
    of::Project project{"Committed", of::Epoch{0}, of::Epoch{100}};
    const auto before = project.snapshot();
    EXPECT_THROW(project.update({.name = "Invalid", .last = of::Epoch{-1}}), of::ProjectError);
    EXPECT_EQ(project.revision(), before.revision);
    EXPECT_EQ(project.name(), "Committed");
    {
        auto edit = project.edit();
        edit.settings("Draft");
        (void)edit.objects().create(of::objects::Satellite{"Uncommitted", {}, {}});
        EXPECT_EQ(project.prepare().snapshot().project->name, "Committed");
        project.save(folder / "committed.orbitforge");
    }
    EXPECT_TRUE(project.objects().find().empty());
    EXPECT_EQ(of::Project::open(folder / "committed.orbitforge").name(), "Committed");
    const auto plan = project.plan_prepare();
    project.update({.name = "New revision"});
    EXPECT_THROW((void)plan.prepare(true), of::RevisionConflictError);
}

TEST_F(ProjectFacade, StaleAnalysisAndFailedSavesLeaveDestinationAndDocumentUntouched) {
    of::Project project{"Saved", of::Epoch{0}, of::Epoch{100}};
    const auto station = project.objects().create(of::objects::GroundStation{"Station", {}, 0});
    const auto analysis = project.prepare();
    const auto path = folder / "saved.orbitforge";
    project.save(path, analysis);
    auto foreign = of::Project::from_data(project.to_data());
    EXPECT_THROW(foreign.save(path, analysis, true), of::ProjectError);
    project.objects().update(station,
                             of::objects::GroundStationUpdate{.minimum_elevation_rad = 0.1});
    const auto changed = project.revision();
    EXPECT_THROW(project.save(path, analysis, true), of::RevisionConflictError);
    EXPECT_THROW(project.save(path), of::ProjectError);
    EXPECT_EQ(project.revision(), changed);
    EXPECT_TRUE(project.dirty());
    auto stored = of::Project::open(path);
    EXPECT_DOUBLE_EQ(std::get<of::application::ProjectGroundStation>(
                         stored.objects().read(stored.objects().get(station.id())))
                         .minimum_elevation_rad,
                     0);
    project.save(path, true);
    EXPECT_FALSE(project.dirty());
    EXPECT_EQ(of::Project::open(path).analysis_status(), of::AnalysisStatus::stale);
}

TEST_F(ProjectFacade, PreparationConsentRetainsDataAndOutlivesFacade) {
    if (!ORBITFORGE_EXPECT_ERFA) {
        GTEST_SKIP();
    }
    const auto plan = [] {
        const auto data = of::DataSnapshot::builtin_time();
        of::application::Project value{"Previous data", of::Epoch{0}, of::Epoch{100}};
        value.time_data = data.time()->data_info();
        value.time_data->id += "-previous";
        auto project = of::Project::from_data(std::move(value));
        try {
            (void)project.prepare(data);
        } catch (const of::DataChangedError &error) {
            return error.plan();
        }
        throw std::runtime_error("Expected explicit preparation consent");
    }();
    ASSERT_EQ(plan.data_changes().size(), 1U);
    EXPECT_THROW((void)plan.prepare(), of::DataChangedError);
    EXPECT_EQ(plan.prepare(true).data().data_lock(), plan.data().data_lock());
}

TEST_F(ProjectFacade, OpeningConsentReturnsFacadeWithoutReopeningOrRememberingAcceptance) {
    if (!ORBITFORGE_EXPECT_ERFA) {
        GTEST_SKIP();
    }
    const auto data = of::DataSnapshot::builtin_time();
    of::Project project{"Recorded", of::Epoch{0}, of::Epoch{100}};
    const auto path = folder / "recorded.orbitforge";
    project.save(path, project.prepare(data));
    EXPECT_THROW((void)of::Project::open(path), of::AnalysisCapabilityError);
    EXPECT_FALSE(of::Project::plan_open(path, data).open().dirty());
    auto stored = project.to_data();
    stored.time_data->id += "-previous";
    of::Project::from_data(stored).save(path, true);
    try {
        (void)of::Project::open(path, data);
        FAIL() << "Expected explicit opening consent";
    } catch (const of::OpenDataChangedError &error) {
        const auto plan = error.plan();
        ASSERT_EQ(plan.data_changes().size(), 1U);
        EXPECT_EQ(plan.snapshot().project->name, "Recorded");
        std::filesystem::remove(path);
        auto opened = plan.open(true);
        EXPECT_TRUE(opened.dirty());
        EXPECT_EQ(opened.snapshot().project->data_lock(), data.data_lock());
        opened.update({.name = "First opening"});
        EXPECT_EQ(plan.open(true).name(), "Recorded");
        EXPECT_THROW((void)plan.open(), of::OpenDataChangedError);
    }
}

TEST_F(ProjectFacade, CatalogOverloadsPinProvidersAndValidateBeforeOpening) {
    if (!of::runtime::data_catalog_available()) {
        GTEST_SKIP();
    }
    of::DataCatalog catalog{folder / "data"};
    const auto entry = catalog.install_eop(ORBITFORGE_REPORT_EOP);
    catalog.activate_eop(entry.lock_sha256);
    of::Project project{"Catalog", of::Epoch{0}, of::Epoch{100}};
    const auto station = project.objects().create(of::objects::GroundStation{"Station", {}, 0});
    const auto planned = project.plan_prepare(catalog).prepare();
    const auto analysis = project.prepare(catalog);
    EXPECT_EQ(planned.data().data_lock(), analysis.data().data_lock());
    EXPECT_EQ(analysis.state(station, of::Epoch{0}, of::FrameId::gcrf).frame, of::FrameId::gcrf);
    const auto path = folder / "catalog.orbitforge";
    project.save(path, analysis);
    const auto plan = of::Project::plan_open(path, catalog);
    EXPECT_FALSE(of::Project::open(path, catalog).dirty());
    std::filesystem::remove(path);
    EXPECT_FALSE(plan.open().dirty());
}
} // namespace
