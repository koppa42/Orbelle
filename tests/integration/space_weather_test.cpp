#include <chrono>
#include <fstream>
#include <gtest/gtest.h>
#include <orbitforge/orbitforge.hpp>
#include <orbitforge/runtime/ephemeris.hpp>
#include <orbitforge/runtime/space_weather.hpp>
#include <orbitforge/runtime/time_frames.hpp>
#include <sstream>
using namespace orbitforge;
namespace {
using Model = core::AtmosphereModel;
using Update = core::GeomagneticUpdate;
using Source = core::GeomagneticSource;
std::vector<core::SpaceWeatherDay> records() {
    std::vector<core::SpaceWeatherDay> result;
    for (int i = 0; i < 14; ++i) {
        core::SpaceWeatherDay d;
        d.date = i < 8 ? core::UtcDateTime{2016, 12, 24 + i, 0, 0, 0, 0}
                       : core::UtcDateTime{2017, 1, i - 7, 0, 0, 0, 0};
        for (int j = 0; j < 8; ++j) {
            d.kp[j] = j / 3.0;
            d.ap[j] = i * 8 + j;
        }
        d.daily_kp = 3;
        d.daily_ap = 100 + i;
        d.f107_observed = 100 + 10 * i;
        d.observed_center81 = 200 + 2 * i;
        d.f107_adjusted = 500;
        d.observed_last81 = 600;
        result.push_back(d);
    }
    return result;
}
class WeatherFiles : public ::testing::Test {
  protected:
    std::filesystem::path root =
        std::filesystem::temp_directory_path() /
        ("of-weather-" +
         std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    void SetUp() override { std::filesystem::create_directories(root); }
    void TearDown() override { std::filesystem::remove_all(root); }
    auto write(const std::string &s) {
        const auto p = root / "weather.csv";
        std::ofstream out(p, std::ios::binary);
        out << s;
        out.close();
        return p;
    }
    const std::string header =
        "DATE,BSRN,ND,KP1,KP2,KP3,KP4,KP5,KP6,KP7,KP8,KP_SUM,"
        "AP1,AP2,AP3,AP4,AP5,AP6,AP7,AP8,AP_AVG,CP,C9,ISN,F10.7_OBS,F10.7_ADJ,"
        "F10.7_DATA_TYPE,F10.7_OBS_CENTER81,F10.7_OBS_LAST81,F10.7_ADJ_CENTER81,F10.7_ADJ_LAST81\n";
    const std::string example =
        "2000-01-01,2272,7,53,47,40,33,43,30,43,37,327,56,39,27,18,32,15,32,22,"
        "30,1.3,6,71,129.9,125.6,OBS,166.2,179.0,161.1,175.0\n";
    std::string daily_file() {
        std::ostringstream out;
        out << header;
        for (const auto &d : records()) {
            out << d.date.year << '-' << (d.date.month < 10 ? "0" : "") << d.date.month << '-'
                << (d.date.day < 10 ? "0" : "") << d.date.day
                << ",1,1,30,30,30,30,30,30,30,30,240,15,15,15,15,15,15,15,15,15,0,0,0,150,500,OBS,"
                   "150,600,500,600\n";
        }
        return out.str();
    }
};
} // namespace
TEST(SpaceWeather, FluxUses20UtcLinearInterpolationAndModelSpecificLags) {
    if (!runtime::time_frames_available())
        GTEST_SKIP();
    const auto time = runtime::make_builtin_time_provider();
    const core::SpaceWeather w{{}, records()};
    const auto at = time->from_utc({2016, 12, 29, 20, 0, 0, 0});
    const auto n = w.sample(at, *time, Model::nrlmsise00, Update::daily, Source::ap);
    EXPECT_DOUBLE_EQ(n.f107, 140);
    EXPECT_DOUBLE_EQ(n.f107_average, 210);
    EXPECT_DOUBLE_EQ(n.ap, 105);
    EXPECT_FALSE(n.ap_history);
    const auto j = w.sample(at, *time, Model::jacchia_roberts, Update::three_hourly, Source::ap);
    EXPECT_DOUBLE_EQ(j.f107, 140);
    EXPECT_DOUBLE_EQ(j.f107_average, 208);
    EXPECT_DOUBLE_EQ(j.ap, 44); // 13:18 UTC after the JR 6.7-hour lag.
    const auto half = w.sample(time->from_utc({2016, 12, 29, 8, 0, 0, 0}), *time, Model::nrlmsise00,
                               Update::daily, Source::kp);
    EXPECT_DOUBLE_EQ(half.f107, 135);
    EXPECT_DOUBLE_EQ(half.f107_average, 209);
    EXPECT_DOUBLE_EQ(half.ap, 15);
}
TEST(SpaceWeather, NativeApHistoryAndKpConversionPrecedeWindowAverages) {
    if (!runtime::time_frames_available())
        GTEST_SKIP();
    const auto time = runtime::make_builtin_time_provider();
    const core::SpaceWeather w{{}, records()};
    const auto at = time->from_utc({2016, 12, 29, 20, 0, 0, 0});
    const auto ap = w.sample(at, *time, Model::nrlmsise00, Update::three_hourly, Source::ap);
    ASSERT_TRUE(ap.ap_history);
    EXPECT_EQ(*ap.ap_history, (std::array<double, 7>{105, 46, 45, 44, 43, 38.5, 30.5}));
    const auto kp = w.sample(at, *time, Model::nrlmsise00, Update::three_hourly, Source::kp);
    ASSERT_TRUE(kp.ap_history);
    const std::array<double, 7> expected{15, 7, 6, 5, 4, 4.5, 4.5};
    for (std::size_t i = 0; i < 7; ++i)
        EXPECT_NEAR((*kp.ap_history)[i], expected[i], 1e-13);
    for (int i = 0; i <= 27; ++i)
        EXPECT_NEAR(core::ap_to_kp(core::kp_to_ap(i / 3.0)), i / 3.0, 1e-14);
    EXPECT_THROW((void)core::ap_to_kp(-1), std::invalid_argument);
    EXPECT_THROW((void)core::ap_to_kp(401), std::invalid_argument);
}
TEST(SpaceWeather, LeapSecondStaysInFinalUtcSlotAndFluxUsesElapsedSeconds) {
    if (!runtime::time_frames_available())
        GTEST_SKIP();
    const auto time = runtime::make_builtin_time_provider();
    const core::SpaceWeather w{{}, records()};
    const auto sample = [&](core::UtcDateTime u) {
        return w.sample(time->from_utc(u), *time, Model::nrlmsise00, Update::three_hourly,
                        Source::ap);
    };
    const auto leap = sample({2016, 12, 31, 23, 59, 60, 0}),
               midnight = sample({2017, 1, 1, 0, 0, 0, 0});
    EXPECT_DOUBLE_EQ(leap.ap, 63);
    EXPECT_DOUBLE_EQ(midnight.ap, 64);
    EXPECT_DOUBLE_EQ(leap.ap_history->at(2), 62);
    EXPECT_DOUBLE_EQ(midnight.ap_history->at(2), 63);
    EXPECT_NEAR(midnight.f107_average, 214 + 2 * 14401.0 / 86401, 1e-12);
    EXPECT_NEAR(midnight.f107, 160 + 10 * 14401.0 / 86400, 1e-12);
}
TEST(SpaceWeather, CoverageChecksInteriorGapsSelectedFieldsAndPrehistory) {
    if (!runtime::time_frames_available())
        GTEST_SKIP();
    const auto time = runtime::make_builtin_time_provider();
    const auto first = time->from_utc({2016, 12, 29, 20, 0, 0, 0});
    const auto last = time->from_utc({2017, 1, 3, 20, 0, 0, 0});
    auto days = records();
    days[7].ap[2] = {};
    days[7].f107_adjusted = {};
    const core::SpaceWeather w{{}, days};
    EXPECT_NO_THROW(
        (void)w.sample(first, *time, Model::nrlmsise00, Update::three_hourly, Source::ap));
    EXPECT_NO_THROW(
        (void)w.sample(last, *time, Model::nrlmsise00, Update::three_hourly, Source::ap));
    EXPECT_THROW((void)w.validate_interval(first, last, *time, Model::nrlmsise00,
                                           Update::three_hourly, Source::ap),
                 core::SpaceWeatherCoverageError);
    EXPECT_NO_THROW((void)w.validate_interval(first, last, *time, Model::nrlmsise00,
                                              Update::three_hourly, Source::kp));
    days = records();
    days.erase(days.begin() + 7);
    const core::SpaceWeather hole{{}, days};
    EXPECT_THROW((void)hole.validate_interval(first, last, *time, Model::jacchia_roberts,
                                              Update::daily, Source::ap),
                 core::SpaceWeatherCoverageError);
    days = records();
    days[5].quality = core::WeatherQuality::monthly_prediction;
    EXPECT_THROW((void)(core::SpaceWeather{{}, days}.sample(first, *time, Model::nrlmsise00,
                                                            Update::daily, Source::ap)),
                 core::SpaceWeatherCoverageError);
    EXPECT_THROW((void)w.sample(time->from_utc({2016, 12, 25, 20, 0, 0, 0}), *time,
                                Model::nrlmsise00, Update::three_hourly, Source::ap),
                 core::SpaceWeatherCoverageError);
    days = records();
    days[1].date = days[0].date;
    EXPECT_THROW((void)(core::SpaceWeather{{}, days}), std::invalid_argument);
}
TEST_F(WeatherFiles, OfficialCsvExampleAndFixedFormatRetainDistinctFields) {
    const auto csv = runtime::load_space_weather(write(header + example));
    const auto &r = csv->days()[0];
    EXPECT_NEAR(*r.kp[0], 16.0 / 3, 1e-14);
    EXPECT_DOUBLE_EQ(*r.daily_kp, 327.0 / 80);
    EXPECT_DOUBLE_EQ(*r.daily_ap, 30);
    EXPECT_DOUBLE_EQ(*r.f107_observed, 129.9);
    EXPECT_DOUBLE_EQ(*r.observed_center81, 166.2);
    EXPECT_DOUBLE_EQ(*r.observed_last81, 179);
    const std::string line = "2000 01 01 2272  7 53 47 40 33 43 30 43 37 327  56  39  27  18  32  "
                             "15  32  22  30 1.3 6  71 125.6 0 161.1 175.0 129.9 166.2 179.0";
    ASSERT_EQ(line.size(), 130U);
    const auto fixed = runtime::load_space_weather(
        write("DATATYPE CssiSpaceWeather\nVERSION 1.2\nNUM_OBSERVED_POINTS 1\nBEGIN OBSERVED\n" +
              line + "\nEND OBSERVED\n"));
    const auto &f = fixed->days()[0];
    EXPECT_EQ(r.kp, f.kp);
    EXPECT_EQ(r.ap, f.ap);
    EXPECT_EQ(r.daily_kp, f.daily_kp);
    EXPECT_EQ(r.f107_observed, f.f107_observed);
    EXPECT_EQ(r.adjusted_last81, f.adjusted_last81);
    EXPECT_NE(csv->info().sha256, fixed->info().sha256);
    auto predicted = example;
    predicted.replace(predicted.find(",53,"), 4, ",22,");
    predicted.replace(predicted.find("OBS"), 3, "PRD");
    EXPECT_DOUBLE_EQ(*runtime::load_space_weather(write(header + predicted))->days()[0].kp[0], 2.2);
    auto missing = example;
    missing.replace(missing.find(",56,"), 4, ",,");
    EXPECT_FALSE(runtime::load_space_weather(write(header + missing))->days()[0].ap[0]);
    EXPECT_THROW((void)runtime::load_space_weather(write(header + example + example)),
                 std::invalid_argument);
    EXPECT_THROW((void)runtime::load_space_weather(write(header + "broken\n")), std::runtime_error);
    EXPECT_THROW((void)runtime::load_space_weather(
                     write("DATATYPE CssiSpaceWeather\nNUM_OBSERVED_POINTS 2\nBEGIN OBSERVED\n" +
                           line + "\nEND OBSERVED\n")),
                 std::runtime_error);
}
TEST_F(WeatherFiles, CatalogPublishesValidatedBytesAndRetainsPreviousOnFailureOrCancellation) {
    if (!runtime::data_catalog_available())
        GTEST_SKIP();
    runtime::DataCatalog catalog(root / "catalog");
    const auto first = catalog.install_space_weather(write(header + example));
    const auto captured = catalog.load_space_weather("celestrak");
    EXPECT_THROW((void)catalog.install_space_weather(write("corrupt")), std::runtime_error);
    EXPECT_EQ(catalog.weather_sources().at(0).info.sha256, first.info.sha256);
    const auto valid = write(header + example + "\n");
    EXPECT_THROW((void)catalog.install_space_weather(valid, "celestrak", "",
                                                     runtime::DataAcquisition::imported,
                                                     [] { throw std::runtime_error("cancel"); }),
                 std::runtime_error);
    EXPECT_EQ(catalog.weather_sources().at(0).info.sha256, first.info.sha256);
    const auto second = catalog.install_space_weather(valid);
    EXPECT_NE(second.info.sha256, first.info.sha256);
    EXPECT_EQ(captured->info().sha256, first.info.sha256);
    EXPECT_DOUBLE_EQ(*captured->days()[0].daily_ap, 30);
    std::ofstream tamper(second.file, std::ios::binary);
    tamper << header << example;
    tamper.close();
    EXPECT_THROW((void)catalog.load_space_weather("celestrak"), std::runtime_error);
}
TEST_F(WeatherFiles, HpopFileWeatherMatchesManualConstantAndPinsSessionReportAndSavedProject) {
    if (!runtime::time_frames_available() || !runtime::ephemeris_available())
        GTEST_SKIP();
    const auto data =
        runtime::DataSnapshot::load(ORBITFORGE_DRAG_EOP)
            .with_ephemeris("de440s",
                            runtime::make_third_body_ephemeris(
                                runtime::load_spice_snapshot(ORBITFORGE_INTEGRATION_SPICE, root)));
    const auto first = data.time()->from_utc({2016, 12, 30, 12, 0, 0, 0});
    const core::Epoch last{first.tai_nanoseconds() + 600000000000};
    application::Project p{"Weather", first, last};
    application::ProjectSatellite satellite;
    auto &orbit = satellite.orbit;
    orbit.first = first;
    orbit.last = last;
    orbit.initial.epoch = first;
    orbit.initial.position_m = {6778137, 0, 0};
    orbit.initial.velocity_mps = {0, 6300, 4300};
    orbit.propagator = application::OrbitPropagator::hpop;
    orbit.hpop.gravity_degree = orbit.hpop.gravity_order = 0;
    orbit.hpop.drag.enabled = true;
    orbit.hpop.drag.atmosphere = Model::nrlmsise00;
    p.satellites.push_back(satellite);
    const auto manual = Project::from_data(p);
    const auto expected =
        manual.prepare(data).state(manual.objects().get("satellite-1"), last, FrameId::gcrf);
    auto &drag = p.satellites[0].orbit.hpop.drag;
    drag.weather_model = "celestrak";
    const auto project = Project::from_data(p);
    EXPECT_THROW((void)project.prepare(data), runtime::AnalysisCapabilityError);
    const auto file = write(daily_file());
    const auto weather = data.with_space_weather_file("celestrak", file);
    const auto analysis = project.prepare(weather);
    const auto object = project.objects().get("satellite-1");
    const auto actual = analysis.state(object, last, FrameId::gcrf);
    EXPECT_EQ(actual.position_m, expected.position_m);
    application::ReportDefinition definition;
    definition.id = "weather";
    definition.name = "Weather";
    definition.query.kind = application::ReportKind::cartesian_state;
    definition.query.object_ids = {object.id()};
    definition.query.frame = FrameId::gcrf;
    definition.query.step_nanoseconds = 300000000000;
    definition.format.time_scale = application::ReportTimeScale::tai;
    const auto report = analysis.report(definition);
    ASSERT_EQ(report.table.rows().size(), 3U);
    for (std::size_t i = 0; i < 3; ++i)
        EXPECT_EQ(std::get<double>(report.table.rows().back()[4 + i]), actual.position_m[i]);
    project.save(root / "weather.orbitforge", analysis);
    const auto reopened = Project::open(root / "weather.orbitforge", weather);
    EXPECT_EQ(reopened.to_data().satellites[0].orbit.hpop.drag, drag);
    EXPECT_EQ(reopened.prepare(weather)
                  .state(reopened.objects().get("satellite-1"), last, FrameId::gcrf)
                  .position_m,
              actual.position_m);
    auto changed = daily_file();
    std::size_t pos = 0;
    while ((pos = changed.find(",150,500,", pos)) != std::string::npos) {
        changed.replace(pos, 9, ",250,500,");
        pos += 9;
    }
    const auto changed_data = data.with_space_weather_file("celestrak", write(changed));
    const auto changed_project = Project::from_data(p);
    EXPECT_NE(changed_project.prepare(changed_data)
                  .state(changed_project.objects().get("satellite-1"), last, FrameId::gcrf)
                  .position_m,
              actual.position_m);
    EXPECT_EQ(analysis.state(object, last, FrameId::gcrf).position_m, actual.position_m);
    EXPECT_THROW((void)reopened.prepare(changed_data), runtime::DataChangedError);
    const auto identity = application::analysis_input_sha256(p);
    drag.f107 = 999;
    drag.kp = 8;
    EXPECT_EQ(application::analysis_input_sha256(p), identity);
    drag.geomagnetic_update = Update::three_hourly;
    EXPECT_NE(application::analysis_input_sha256(p), identity);
}
