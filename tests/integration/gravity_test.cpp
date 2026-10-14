#include <fstream>
#include <gtest/gtest.h>
#include <orbitforge/runtime/analysis.hpp>
#include <orbitforge/runtime/data_catalog.hpp>
#include <orbitforge/runtime/gravity.hpp>
#include <orbitforge/runtime/project.hpp>
#include <orbitforge/runtime/time_frames.hpp>
#include <sstream>
using namespace orbitforge;
namespace {
class GravityFiles : public ::testing::Test {
  protected:
    std::filesystem::path root =
        std::filesystem::temp_directory_path() /
        ("orbitforge-gravity-" +
         std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    void SetUp() override { std::filesystem::create_directories(root); }
    void TearDown() override { std::filesystem::remove_all(root); }
    std::filesystem::path write(const std::string &bytes) {
        const auto path = root / "input.grv";
        std::ofstream out(path, std::ios::binary);
        out << bytes;
        return path;
    }
    std::string grv(std::string value = "-4.84165371736e-4", std::string norm = "Yes") {
        return "stk.v.6.0\nModel Test\nDegree 2\nOrder 2\nGm 3.986004415e14\nRefDistance "
               "6378136.3\n"
               "Normalized " +
               norm + "\nIncludesPermTide No\nBEGIN Coefficients\n2 0 " + value +
               " 0\nEND Coefficients\n";
    }
};
} // namespace
TEST_F(GravityFiles, ParsesStaticFormatsNormalizationAndHashesOriginalBytes) {
    const auto field = runtime::load_gravity(write(grv()));
    const auto unnormalized = runtime::load_gravity(write(grv("-1.0826266835531513e-3", "No")));
    const auto a = field->acceleration({4e6, -3e6, 5e6}, 2, 0);
    const auto b = unnormalized->acceleration({4e6, -3e6, 5e6}, 2, 0);
    for (int i = 0; i < 3; ++i)
        EXPECT_NEAR(a[i], b[i], 1e-14);
    EXPECT_EQ(field->info().degree, 2);
    const auto icgem = runtime::load_gravity(
        write("product_type gravity_field\nmodelname Test\nearth_gravity_constant 3.986004415e14\n"
              "radius 6378136.3\nmax_degree 2\ntide_system tide_free\nend_of_head\n"
              "gfc 0 0 1 0\ngfc 2 0 -4.84165371736D-4 0\ngfc 2 1 0 0\ngfc 2 2 0 0\n"));
    EXPECT_EQ(icgem->acceleration({4e6, -3e6, 5e6}, 2, 2), a);
    auto crlf = grv();
    for (std::size_t at = 0; (at = crlf.find('\n', at)) != std::string::npos; at += 2)
        crlf.insert(at, 1, '\r');
    EXPECT_NE(runtime::load_gravity(write(crlf))->info().sha256, field->info().sha256);
}
TEST_F(GravityFiles, RejectsTruncationTimeDependenceDuplicatesAndUnsupportedTides) {
    for (const auto &bad :
         {grv() + "BEGIN EarthIERSTideModel\n", grv("nan"), grv("-0.001", "Sometimes"),
          grv().substr(0, grv().find("END Coefficients")),
          grv().substr(0, grv().find("END Coefficients")) + "2 0 0 0\nEND Coefficients\n",
          std::string("product_type gravity_field\nmodelname T\nearth_gravity_constant "
                      "3.986004415e14\nradius 6378136.3\nmax_degree 2\ntide_system "
                      "tide_free\nend_of_head\ngfct 2 0 0 0\n")})
        EXPECT_THROW((void)runtime::load_gravity(write(bad)), std::runtime_error);
}
TEST_F(GravityFiles, CatalogFailureAndCancellationPreserveCurrentAndRunningResources) {
    if (!runtime::data_catalog_available())
        GTEST_SKIP();
    runtime::DataCatalog catalog(root / "catalog");
    const auto installed = catalog.install_gravity(write(grv()), "custom");
    auto frozen = catalog.load_gravity("custom");
    EXPECT_EQ(installed.info.sha256, frozen->info().sha256);
    EXPECT_THROW((void)catalog.install_gravity(write(grv()), "egm2008"), std::runtime_error);
    EXPECT_THROW((void)catalog.install_gravity(write(grv("-0.0006")), "custom", {},
                                               runtime::DataAcquisition::imported,
                                               [] { throw std::runtime_error("cancelled"); }),
                 std::runtime_error);
    EXPECT_EQ(catalog.load_gravity("custom")->info().sha256, installed.info.sha256);
    catalog.install_gravity(write(grv("-0.0006")), "custom");
    EXPECT_NE(catalog.load_gravity("custom")->info().sha256, frozen->info().sha256);
    EXPECT_EQ(frozen->info().sha256, installed.info.sha256);
    EXPECT_THROW((void)catalog.install_gravity(write(grv()), "../bad"), std::runtime_error);
}
TEST_F(GravityFiles, HpopSnapshotProjectRoundtripAndChangedDataConfirmation) {
    if (!runtime::time_frames_available())
        GTEST_SKIP();
    auto data = runtime::DataSnapshot::load(ORBITFORGE_REPORT_EOP)
                    .with_gravity_file("custom", write(grv()));
    application::Project p{"Gravity", core::Epoch{0}, core::Epoch{600'000'000'000}};
    application::ProjectSatellite s;
    s.orbit.first = p.first;
    s.orbit.last = p.last;
    s.orbit.propagator = application::OrbitPropagator::hpop;
    s.orbit.hpop.gravity_model = "custom";
    s.orbit.hpop.gravity_degree = s.orbit.hpop.gravity_order = 2;
    p.satellites.push_back(s);
    application::ProjectDocument project{p};
    const auto analysis = runtime::prepare(project, data);
    const auto ref = project.objects().get("satellite-1");
    const auto original = analysis.state(ref, p.last, core::FrameId::gcrf);
    runtime::save_project(project, root / "scenario.orbitforge", analysis);
    const auto stored = application::load_project(root / "scenario.orbitforge");
    EXPECT_EQ(stored.force_data.size(), 1U);
    EXPECT_EQ(stored.data_lock(), data.data_lock());
    runtime::ReportContext context{stored, data.time(), data.frames(), data.eop(),
                                   data.gravity_models()};
    EXPECT_NO_THROW((void)context.force_environment(s.orbit));
    EXPECT_NO_THROW((void)runtime::make_project_scenario(stored, data.time().get(),
                                                         data.force_environment(s.orbit)));
    auto next = data.with_gravity_file("custom", write(grv("-0.0006")));
    context.gravity = next.gravity_models();
    EXPECT_THROW((void)context.force_environment(s.orbit), std::runtime_error);
    EXPECT_THROW((void)runtime::make_project_scenario(stored, data.time().get(),
                                                      next.force_environment(s.orbit)),
                 std::runtime_error);
    runtime::ProjectOpenPlan plan(root / "scenario.orbitforge", next);
    ASSERT_EQ(plan.data_changes().size(), 1U);
    EXPECT_EQ(plan.data_changes()[0].role, "gravity/custom");
    EXPECT_THROW((void)plan.open(), runtime::OpenDataChangedError);
    EXPECT_NO_THROW((void)plan.open(true));
    EXPECT_EQ(analysis.state(ref, p.last, core::FrameId::gcrf).position_m, original.position_m);
    EXPECT_THROW(
        (void)runtime::prepare(project, runtime::DataSnapshot::load(ORBITFORGE_REPORT_EOP)),
        runtime::AnalysisCapabilityError);
}
