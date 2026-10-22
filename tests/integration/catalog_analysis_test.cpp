#include <gtest/gtest.h>
#include <orbelle/runtime/data_catalog.hpp>
#include <orbelle/runtime/ephemeris.hpp>
#include <orbelle/runtime/time_frames.hpp>

using namespace orbelle;
namespace {
class CatalogFolder {
  public:
    std::filesystem::path path;
    CatalogFolder() {
        const auto parent = std::filesystem::path(ORBELLE_INTEGRATION_WORKSPACE).parent_path();
        path =
            parent / ("catalog-analysis-" +
                      std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
        std::filesystem::create_directories(path);
    }
    ~CatalogFolder() {
        const auto parent = std::filesystem::path(ORBELLE_INTEGRATION_WORKSPACE).parent_path();
        if (path.is_absolute() && path.parent_path() == parent &&
            path.filename().string().starts_with("catalog-analysis-")) {
            std::error_code error;
            std::filesystem::remove_all(path, error);
        }
    }
};
class RestrictedEop final : public core::IEarthOrientationProvider {
  public:
    std::shared_ptr<const core::IEarthOrientationProvider> source;
    core::EopDataInfo info;
    explicit RestrictedEop(std::shared_ptr<const core::IEarthOrientationProvider> provider)
        : source(std::move(provider)), info(source->data_info()) {
        info.first_epoch = core::Epoch{info.first_epoch.tai_nanoseconds() + 3600'000'000'000};
        info.last_epoch = core::Epoch{info.last_epoch.tai_nanoseconds() - 3600'000'000'000};
    }
    const core::EopDataInfo &data_info() const noexcept override { return info; }
    core::EarthOrientationState at(core::Epoch epoch) const override { return source->at(epoch); }
};
} // namespace
TEST(CatalogAnalysis, CapturesCurrentResourcesAndUsesPublicPrepare) {
    if (!runtime::data_catalog_available())
        GTEST_SKIP();
    CatalogFolder folder;
    const auto fixed = [&] {
        runtime::DataCatalog catalog(folder.path);
        const auto entry = catalog.install_eop(ORBELLE_REPORT_EOP);
        catalog.activate_eop(entry.lock_sha256);
        catalog.set_extrapolation_days(runtime::EopProduct::c04, 3);
        application::ProjectDocument project{
            application::Project{"Catalog", core::Epoch{0}, core::Epoch{10}}};
        const auto station =
            project.objects().create(application::objects::GroundStation{"Station", {}, 0});
        const auto analysis = runtime::prepare(project, catalog);
        EXPECT_EQ(analysis.data().data_lock().sha256(), entry.lock_sha256);
        EXPECT_EQ(analysis.data().eop()->data_info().extrapolation_days, 0);
        EXPECT_EQ(analysis.state(station, core::Epoch{0}, core::FrameId::gcrf).frame,
                  core::FrameId::gcrf);
        runtime::AnalysisPlan plan{project, catalog};
        project.objects().rename(station, "Changed");
        EXPECT_THROW((void)plan.prepare(), application::RevisionConflictError);
        return analysis.data();
    }();
    EXPECT_NO_THROW((void)fixed.eop()->at(core::Epoch{0}));
    EXPECT_NO_THROW((void)fixed.time()->to_utc(core::Epoch{0}));
}
TEST(CatalogAnalysis, FullCoveragePrecedesExtrapolationAndUnusedPolicyDoesNotChangeIdentity) {
    if (!runtime::time_frames_available())
        GTEST_SKIP();
    const std::shared_ptr<const core::ITimeProvider> time = runtime::make_builtin_time_provider();
    const std::shared_ptr<const core::IEarthOrientationProvider> full =
        runtime::load_eop_snapshot(ORBELLE_REPORT_EOP);
    const auto limited = std::make_shared<RestrictedEop>(full);
    const std::array candidates{runtime::EopCandidate{limited, 3}, runtime::EopCandidate{full, 3}};
    const auto first = full->data_info().first_epoch, last = full->data_info().last_epoch;
    const auto selected = runtime::select_eop_for_interval(candidates, first, last, time);
    EXPECT_EQ(selected, full);
    EXPECT_EQ(selected->data_info().extrapolation_days, 0);
    const auto inside = runtime::select_eop_for_interval(candidates, limited->info.first_epoch,
                                                         limited->info.last_epoch, time);
    EXPECT_EQ(inside, limited);
    const auto after = core::Epoch{last.tai_nanoseconds() + 3600'000'000'000};
    const auto extrapolated = runtime::select_eop_for_interval(candidates, first, after, time);
    EXPECT_EQ(extrapolated->data_info().extrapolation_days, 3);
    EXPECT_NO_THROW((void)extrapolated->at(after));
    EXPECT_THROW((void)runtime::select_eop_for_interval(candidates, after, first, time),
                 std::invalid_argument);
    EXPECT_THROW((void)runtime::select_eop_for_interval({}, first, last, time),
                 core::EopCoverageError);
}

TEST(CatalogAnalysis, LightingLoadsSavedOrRecordedSolarResourceWithoutSrp) {
    if (!runtime::data_catalog_available() || !runtime::ephemeris_available())
        GTEST_SKIP();
    CatalogFolder folder;
    runtime::DataCatalog catalog(folder.path);
    const auto eop = catalog.install_eop(ORBELLE_DRAG_EOP);
    catalog.activate_eop(eop.lock_sha256);
    const auto solar = catalog.install_ephemeris_snapshot(ORBELLE_INTEGRATION_PLANETS);
    const auto first = core::Epoch{solar.info.first.tai_nanoseconds() + 600000000000LL};
    application::Project input{"Lighting", first,
                               core::Epoch{first.tai_nanoseconds() + 60000000000LL}};
    application::ProjectSatellite sat;
    sat.orbit.initial.epoch = first;
    sat.orbit.first = input.first;
    sat.orbit.last = input.last;
    input.satellites.push_back(sat);
    application::ReportDefinition report;
    report.id = report.name = "lighting";
    report.query.kind = application::ReportKind::lighting_intervals;
    report.query.object_ids = {sat.orbit.object_id};
    report.format.time_scale = application::ReportTimeScale::tai;
    input.reports.push_back(report);
    application::ProjectDocument document{input};
    const auto analysis = runtime::prepare(document, catalog);
    EXPECT_TRUE(analysis.data().ephemerides().contains("de440s"));
    EXPECT_FALSE(analysis.report(report).table.rows().empty());
    const auto file = folder.path / "lighting.orbelle";
    runtime::save_project(document, file, analysis);
    auto recorded = application::load_project(file);
    recorded.reports.clear();
    const auto repeated = runtime::prepare(application::ProjectDocument{recorded}, catalog);
    EXPECT_EQ(repeated.report(report).table.rows(), analysis.report(report).table.rows());
}
