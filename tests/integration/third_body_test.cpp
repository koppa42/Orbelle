#include <fstream>
#include <gtest/gtest.h>
#include <orbitforge/orbitforge.hpp>
#include <orbitforge/runtime/ephemeris.hpp>
#include <orbitforge/runtime/hpop.hpp>

using namespace orbitforge;
namespace {
class EphemerisFiles : public ::testing::Test {
  protected:
    std::filesystem::path root =
        std::filesystem::temp_directory_path() /
        ("orbitforge-ephemeris-" +
         std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    void SetUp() override { std::filesystem::create_directories(root); }
    void TearDown() override { std::filesystem::remove_all(root); }
};
} // namespace
TEST_F(EphemerisFiles, SunMoonUseRealSpicePositionsAndRecordedIdentityAcrossReportsAndReopen) {
    if (!runtime::ephemeris_available())
        GTEST_SKIP();
    auto raw = runtime::load_spice_snapshot(ORBITFORGE_INTEGRATION_SPICE, root);
    const auto epoch = raw->data_info().first_epoch;
    const auto ephemeris = runtime::make_third_body_ephemeris(std::move(raw));
    EXPECT_NO_THROW(ephemeris->validate_interval(epoch, ephemeris->data_info().last, true, true));
    const auto data = runtime::DataSnapshot::builtin_time().with_ephemeris("de440s", ephemeris);
    application::Project input{"Sun and Moon", epoch,
                               core::Epoch{epoch.tai_nanoseconds() + 600000000000}};
    application::ProjectSatellite satellite;
    satellite.orbit.initial.epoch = epoch;
    satellite.orbit.propagator = application::OrbitPropagator::hpop;
    satellite.orbit.hpop.gravity_degree = satellite.orbit.hpop.gravity_order = 0;
    satellite.orbit.hpop.sun_gravity = satellite.orbit.hpop.moon_gravity = true;
    satellite.orbit.first = input.first;
    satellite.orbit.last = input.last;
    input.satellites.push_back(satellite);
    auto project = Project::from_data(input);
    EXPECT_THROW((void)project.prepare(), runtime::AnalysisCapabilityError);
    const auto analysis = project.prepare(data);
    const auto sat = project.objects().get("satellite-1");
    const auto state = analysis.state(sat, input.last, FrameId::gcrf);
    project.save(root / "test.orbitforge", analysis);
    const auto restored = Project::open(root / "test.orbitforge", data);
    EXPECT_EQ(restored.to_data().satellites[0].orbit.hpop, satellite.orbit.hpop);
    EXPECT_EQ(restored.to_data().force_data.size(), 1U);
    EXPECT_EQ(restored.to_data().force_data[0].role, "ephemeris/de440s");
    const auto session = runtime::make_hpop_session(satellite.orbit.initial, satellite.orbit.hpop,
                                                    data.force_environment(satellite.orbit));
    EXPECT_NE(session.provenance().algorithm.find("ephemeris="), std::string::npos);
    EXPECT_EQ(session.sample(std::array{input.last}).states()[0].position_m, state.position_m);
    auto changed = input;
    changed.satellites[0].orbit.initial.epoch = core::Epoch{epoch.tai_nanoseconds() - 1};
    EXPECT_THROW((void)Project::from_data(changed).prepare(data), core::EphemerisCoverageError);
    auto original = application::analysis_input_sha256(input);
    input.satellites[0].orbit.hpop.sun_mu = runtime::de440_sun_mu * 1.01;
    EXPECT_NE(application::analysis_input_sha256(input), original);
    EXPECT_EQ(
        application::decode_project(application::encode_project(input)).satellites[0].orbit.hpop,
        input.satellites[0].orbit.hpop);
    input.satellites[0].orbit.hpop.sun_gravity = false;
    original = application::analysis_input_sha256(input);
    auto report_original = runtime::report_input_sha256(input);
    input.satellites[0].orbit.hpop.sun_mu = runtime::de440_sun_mu * 1.02;
    EXPECT_EQ(application::analysis_input_sha256(input), original);
    EXPECT_EQ(runtime::report_input_sha256(input), report_original);
    input.satellites[0].orbit.hpop.moon_gravity = false;
    original = application::analysis_input_sha256(input);
    report_original = runtime::report_input_sha256(input);
    input.satellites[0].orbit.hpop.moon_mu = 1;
    input.satellites[0].orbit.hpop.ephemeris_model = "unused-draft";
    EXPECT_EQ(application::analysis_input_sha256(input), original);
    EXPECT_EQ(runtime::report_input_sha256(input), report_original);
}
TEST_F(EphemerisFiles, CatalogRejectsInvalidOrCancelledInstallWithoutPublishing) {
    if (!runtime::data_catalog_available() || !runtime::ephemeris_available())
        GTEST_SKIP();
    runtime::DataCatalog catalog(root / "catalog");
    const auto file = root / "bad.bsp";
    std::ofstream(file, std::ios::binary) << "not a full DE440s file";
    EXPECT_THROW((void)catalog.install_de440s(file), std::runtime_error);
    EXPECT_TRUE(catalog.ephemerides().empty());
    EXPECT_THROW((void)catalog.install_de440s(file, runtime::DataAcquisition::imported,
                                              [] { throw std::runtime_error("cancel"); }),
                 std::runtime_error);
    EXPECT_TRUE(catalog.ephemerides().empty());
    EXPECT_THROW((void)catalog.load_ephemeris("de440s"), std::runtime_error);
    EXPECT_TRUE(std::filesystem::is_empty(catalog.root() / "ephemerides"));
    EXPECT_TRUE(std::filesystem::is_empty(catalog.root() / "staging"));
}
