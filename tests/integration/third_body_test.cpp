#include <cmath>
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
    satellite.orbit.first = input.first;
    satellite.orbit.last = input.last;
    satellite.orbit.hpop.gravity_degree = satellite.orbit.hpop.gravity_order = 0;
    satellite.orbit.hpop.third_body(orbitforge::core::ThirdBody::sun).enabled =
        satellite.orbit.hpop.third_body(orbitforge::core::ThirdBody::moon).enabled = true;
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
    input.satellites[0].orbit.hpop.third_body(core::ThirdBody::sun).source =
        core::ThirdBodyGravitySource::user_specified;
    input.satellites[0].orbit.hpop.third_body(core::ThirdBody::sun).user_mu =
        runtime::de440_sun_mu * 1.01;
    EXPECT_NE(application::analysis_input_sha256(input), original);
    EXPECT_EQ(
        application::decode_project(application::encode_project(input)).satellites[0].orbit.hpop,
        input.satellites[0].orbit.hpop);
    input.satellites[0].orbit.hpop.third_body(orbitforge::core::ThirdBody::sun).enabled = false;
    original = application::analysis_input_sha256(input);
    auto report_original = runtime::report_input_sha256(input);
    input.satellites[0].orbit.hpop.third_body(core::ThirdBody::sun).user_mu =
        runtime::de440_sun_mu * 1.02;
    EXPECT_EQ(application::analysis_input_sha256(input), original);
    EXPECT_EQ(runtime::report_input_sha256(input), report_original);
    input.satellites[0].orbit.hpop.third_body(orbitforge::core::ThirdBody::moon).enabled = false;
    original = application::analysis_input_sha256(input);
    report_original = runtime::report_input_sha256(input);
    input.satellites[0].orbit.hpop.third_body(core::ThirdBody::moon).user_mu = 1;
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
TEST_F(EphemerisFiles, CbConstantsSelectSystemGmAndSurviveReopenWithoutTheSourceFile) {
    const auto file = root / "Jupiter.cb";
    std::ofstream(file)
        << "BEGIN CentralBody\nGm 1.266865319e17\nSystemGm 1.26712764D17\nEND CentralBody\n";
    const auto cb = runtime::load_cb_gravity(file, core::ThirdBody::jupiter);
    EXPECT_EQ(cb.mu, 1.26712764e17);
    EXPECT_EQ(cb.body, core::ThirdBody::jupiter);
    EXPECT_EQ(cb.content_sha256.size(), 64U);
    EXPECT_THROW((void)runtime::load_cb_gravity(file, core::ThirdBody::sun), std::invalid_argument);
    application::Project input{"Constants", core::Epoch{0}, core::Epoch{600000000000}};
    application::ProjectSatellite satellite;
    satellite.orbit.propagator = application::OrbitPropagator::hpop;
    satellite.orbit.first = input.first;
    satellite.orbit.last = input.last;
    auto &settings = satellite.orbit.hpop.third_body(core::ThirdBody::jupiter);
    settings = {true, core::ThirdBodyGravitySource::cb_file, 1.25e17, cb};
    input.satellites.push_back(satellite);
    const auto encoded = application::encode_project(input);
    std::filesystem::remove(file);
    EXPECT_EQ(application::decode_project(encoded).satellites[0].orbit.hpop, satellite.orbit.hpop);
    auto hash = application::analysis_input_sha256(input);
    auto &row = input.satellites[0].orbit.hpop.third_body(core::ThirdBody::jupiter);
    row.user_mu = 1.3e17; // Inactive manual draft cannot invalidate a Cb calculation.
    EXPECT_EQ(application::analysis_input_sha256(input), hash);
    row.cb->content_sha256[0] = row.cb->content_sha256[0] == 'a' ? 'b' : 'a';
    EXPECT_NE(application::analysis_input_sha256(input), hash);
    row.enabled = false;
    hash = application::analysis_input_sha256(input);
    row.source = core::ThirdBodyGravitySource::jpl_de;
    EXPECT_EQ(application::analysis_input_sha256(input), hash);
    std::ofstream(file) << "Gm 1e17\n";
    EXPECT_THROW((void)runtime::load_cb_gravity(file, core::ThirdBody::jupiter),
                 std::invalid_argument);
    std::ofstream(file) << "SystemGm 1.26712764e8\n";
    EXPECT_EQ(runtime::load_cb_gravity(file, core::ThirdBody::jupiter, 1e9).mu, cb.mu);
    std::ofstream(file) << "SystemGm nan\n";
    EXPECT_THROW((void)runtime::load_cb_gravity(file, core::ThirdBody::jupiter),
                 std::invalid_argument);
    std::ofstream(file) << "SystemGm 1e17\nSystemGm 2e17\n";
    EXPECT_THROW((void)runtime::load_cb_gravity(file, core::ThirdBody::jupiter),
                 std::invalid_argument);
    const auto venus = root / "Venus.cb", sun = root / "Sun.cb";
    std::ofstream(venus) << "Gm 3e14\nSystemGm 3.25e14\n";
    EXPECT_EQ(runtime::load_cb_gravity(venus, core::ThirdBody::venus).mu, 3.25e14);
    std::ofstream(sun) << "Gm 1.327e20\nSystemGm 1.4e20\n";
    EXPECT_EQ(runtime::load_cb_gravity(sun, core::ThirdBody::sun).mu, 1.327e20);
}
TEST_F(EphemerisFiles, EveryPlanetUsesTheDeclaredCentreAndFullIntegrationCoverage) {
    if (!runtime::ephemeris_available())
        GTEST_SKIP();
    auto raw = runtime::load_spice_snapshot(ORBITFORGE_INTEGRATION_PLANETS, root);
    const auto first = raw->data_info().first_epoch, last = raw->data_info().last_epoch;
    const auto ephemeris = runtime::make_third_body_ephemeris(std::move(raw));
    EXPECT_NO_THROW(ephemeris->validate_interval(first, last, core::third_body_list));
    auto original = runtime::load_spice_snapshot(ORBITFORGE_INTEGRATION_SPICE, root);
    const auto moon_only = runtime::make_third_body_ephemeris(std::move(original));
    EXPECT_THROW(moon_only->validate_interval(first, last, core::third_body_list),
                 core::EphemerisCoverageError);
    const auto data = runtime::DataSnapshot::builtin_time().with_ephemeris("de440s", ephemeris);
    application::Project input{"Planets", first,
                               core::Epoch{first.tai_nanoseconds() + 600000000000}};
    application::ProjectSatellite sat;
    sat.orbit.propagator = application::OrbitPropagator::hpop;
    sat.orbit.initial.epoch = first;
    sat.orbit.first = input.first;
    sat.orbit.last = input.last;
    sat.orbit.hpop.gravity_degree = sat.orbit.hpop.gravity_order = 0;
    for (auto body : core::third_body_list) {
        sat.orbit.hpop.third_body(body).enabled = true;
        const auto position = ephemeris->position(body, first);
        EXPECT_GT(std::hypot(position[0], position[1], position[2]), 3e8);
        EXPECT_GT(ephemeris->data_info().mu(body), 0);
    }
    input.satellites.push_back(sat);
    auto project = Project::from_data(input);
    const auto analysis = project.prepare(data);
    const auto object = project.objects().get("satellite-1");
    const auto state = analysis.state(object, input.last, FrameId::gcrf);
    EXPECT_TRUE(std::isfinite(state.position_m[0]));
    project.save(root / "planets.orbitforge", analysis);
    const auto reopened = Project::open(root / "planets.orbitforge", data);
    EXPECT_EQ(reopened.to_data().satellites[0].orbit.hpop, sat.orbit.hpop);
    EXPECT_EQ(reopened.prepare(data)
                  .state(reopened.objects().get("satellite-1"), input.last, FrameId::gcrf)
                  .position_m,
              state.position_m);
}
