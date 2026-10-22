#include <array>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <future>
#include <gtest/gtest.h>
#include <iomanip>
#include <limits>
#include <nlohmann/json.hpp>
#include <numbers>
#include <orbelle/core/propagation/circular_two_body.hpp>
#include <orbelle/providers/erfa/provider.hpp>
#include <orbelle/providers/iers/eop_snapshot.hpp>
#include <picosha2.h>
#include <sstream>

using namespace orbelle;
namespace {
constexpr double arcsec = std::numbers::pi / (180 * 3600);
auto load(const core::ITimeProvider &time) {
    return providers::iers::load_eop_snapshot(ORBELLE_EOP_SAMPLE, time);
}
core::Epoch shift(core::Epoch epoch, double seconds) {
    return core::Epoch(epoch.tai_nanoseconds() +
                       static_cast<std::int64_t>(std::llround(seconds * 1e9)));
}
std::string read(const std::filesystem::path &path) {
    std::ifstream stream(path, std::ios::binary);
    return {std::istreambuf_iterator<char>(stream), {}};
}
std::filesystem::path fixture(const std::string &data, nlohmann::json manifest) {
    const auto dir = std::filesystem::path(ORBELLE_EOP_TEST_DIR) /
                     ::testing::UnitTest::GetInstance()->current_test_info()->name();
    std::filesystem::create_directories(dir);
    manifest["sha256"] = picosha2::hash256_hex_string(data);
    manifest["data_file"] = "eop.c04";
    std::ofstream(dir / "eop.c04", std::ios::binary) << data;
    std::ofstream(dir / "manifest.json") << manifest.dump(2);
    return dir / "manifest.json";
}
double polynomial(double t) { return 0.1 + 1e-9 * t + 2e-15 * t * t - 1e-21 * t * t * t; }
double polynomial_rate(double t) { return 1e-9 + 4e-15 * t - 3e-21 * t * t; }
} // namespace

TEST(IersEop, PublishedNodesUseSiUnitsAndLeapOffsets) {
    const auto time = providers::erfa::make_builtin_time_provider();
    const auto table = load(*time);
    const auto before = time->from_utc({2016, 12, 31, 0, 0, 0, 0});
    const auto after = time->from_utc({2017, 1, 1, 0, 0, 0, 0});
    const auto a = table->at(before), b = table->at(after);
    EXPECT_EQ(after.seconds_since(before), 86401);
    EXPECT_NEAR(a.sample.ut1_minus_tai_seconds, -36.4077697, 1e-12);
    EXPECT_NEAR(b.sample.ut1_minus_tai_seconds, -36.4087130, 1e-12);
    EXPECT_NEAR(a.sample.polar_motion_x_rad, 0.081440 * arcsec, 1e-18);
    EXPECT_NEAR(a.sample.polar_motion_y_rad, 0.263099 * arcsec, 1e-18);
    EXPECT_NEAR(a.sample.celestial_pole_dx_rad, 0.000106 * arcsec, 1e-18);
    EXPECT_NEAR(a.sample.celestial_pole_dy_rad, -0.000192 * arcsec, 1e-18);
    EXPECT_NEAR(a.rates.ut1_minus_tai_seconds_per_second, -0.0008920 / 86400, 1e-20);
    EXPECT_NEAR(a.rates.polar_motion_x_rad_per_second, -0.001260 * arcsec / 86400, 1e-24);
    EXPECT_NE(a.sample.source_id.find(table->data_info().sha256), std::string::npos);
    EXPECT_EQ(table->data_info().time_data.sha256, time->data_info().sha256);
    const auto end_of_leap = table->at(shift(after, -1e-9));
    EXPECT_NEAR(end_of_leap.sample.ut1_minus_tai_seconds, b.sample.ut1_minus_tai_seconds, 1e-12);
    EXPECT_NEAR(end_of_leap.rates.ut1_minus_tai_seconds_per_second,
                b.rates.ut1_minus_tai_seconds_per_second, 1e-18);
}
TEST(IersImport, ConcurrentImportsAreIdempotentAndIndependentOfTheSource) {
    const auto time = providers::erfa::make_builtin_time_provider();
    const auto original_manifest = nlohmann::json::parse(read(ORBELLE_EOP_SAMPLE));
    const auto original_data =
        read(std::filesystem::path(ORBELLE_EOP_SAMPLE).parent_path() / "eop.c04");
    const auto source = fixture(original_data, original_manifest);
    const auto store = source.parent_path() / "store";
    auto first = std::async(std::launch::async, [&] {
        return providers::iers::import_eop_snapshot(source, store, *time);
    });
    auto second = std::async(std::launch::async, [&] {
        return providers::iers::import_eop_snapshot(source, store, *time);
    });
    const auto saved = first.get();
    EXPECT_EQ(saved, second.get());
    EXPECT_EQ(read(saved), read(source));
    EXPECT_EQ(read(saved.parent_path() / "eop.c04"), original_data);
    std::ofstream(source.parent_path() / "eop.c04") << "changed source";
    EXPECT_NO_THROW((void)providers::iers::load_eop_snapshot(saved, *time));
    EXPECT_THROW((void)providers::iers::import_eop_snapshot(source, store, *time),
                 core::EopDataError);
    EXPECT_EQ(read(saved.parent_path() / "eop.c04"), original_data);
    std::ofstream(source.parent_path() / "eop.c04", std::ios::binary) << original_data;
    // Corrupted installed versions are reported; importing must never silently replace one.
    std::ofstream(saved.parent_path() / "eop.c04") << "corrupted installed bytes";
    EXPECT_THROW((void)providers::iers::import_eop_snapshot(source, store, *time),
                 core::EopDataError);
    EXPECT_EQ(read(saved.parent_path() / "eop.c04"), "corrupted installed bytes");
    std::ofstream(saved.parent_path() / "eop.c04", std::ios::binary) << original_data;
    EXPECT_EQ(std::distance(std::filesystem::directory_iterator(store),
                            std::filesystem::directory_iterator{}),
              1);
}
TEST(IersImport, FinalsSnapshotAndInvalidManifestNeverPublishPartialData) {
    const auto time = providers::erfa::make_builtin_time_provider();
    auto manifest = nlohmann::json::parse(read(ORBELLE_FINALS_SAMPLE));
    const auto bytes =
        read(std::filesystem::path(ORBELLE_FINALS_SAMPLE).parent_path() / "finals2000A.data");
    const auto source = fixture(bytes, manifest);
    const auto store = source.parent_path() / "store";
    const auto saved = providers::iers::import_eop_snapshot(source, store, *time);
    EXPECT_EQ(providers::iers::load_eop_snapshot(saved, *time)->data_info().interpolation,
              "finals2000a-hermite-tai-v1");
    manifest["data_file"] = "../outside.data";
    std::ofstream(source) << manifest.dump();
    EXPECT_THROW((void)providers::iers::import_eop_snapshot(source, store, *time),
                 core::EopDataError);
    EXPECT_EQ(std::distance(std::filesystem::directory_iterator(store),
                            std::filesystem::directory_iterator{}),
              1);
}

TEST(IersFinals, PublishedPredictedValuesUnitsProvenanceAndNoExtrapolation) {
    const auto time = providers::erfa::make_builtin_time_provider();
    const auto table = providers::iers::load_eop_snapshot(ORBELLE_FINALS_SAMPLE, *time);
    const auto epoch = time->from_utc({2026, 10, 9, 0, 0, 0, 0});
    const auto sample = table->at(epoch).sample;
    EXPECT_NEAR(sample.ut1_minus_tai_seconds, -37.0302856, 1e-12);
    EXPECT_NEAR(sample.polar_motion_x_rad, 0.167231 * arcsec, 1e-18);
    EXPECT_NEAR(sample.polar_motion_y_rad, 0.322270 * arcsec, 1e-18);
    EXPECT_NEAR(sample.celestial_pole_dx_rad, 0.000146 * arcsec, 1e-18);
    EXPECT_NEAR(sample.celestial_pole_dy_rad, 0.000259 * arcsec, 1e-18);
    EXPECT_NE(sample.source_id.find("prediction-mask=7"), std::string::npos);
    EXPECT_EQ(table->data_info().interpolation, "finals2000a-hermite-tai-v1");
    EXPECT_THROW((void)table->at(shift(table->data_info().last_epoch, 1e-9)),
                 core::EopCoverageError);
    const auto middle = shift(epoch, 3600);
    const auto state = table->at(middle), before = table->at(shift(middle, -1)),
               after = table->at(shift(middle, 1));
    EXPECT_NEAR(state.rates.ut1_minus_tai_seconds_per_second,
                (after.sample.ut1_minus_tai_seconds - before.sample.ut1_minus_tai_seconds) / 2,
                1e-13);
    EXPECT_NEAR(state.rates.polar_motion_x_rad_per_second,
                (after.sample.polar_motion_x_rad - before.sample.polar_motion_x_rad) / 2, 1e-20);
}

TEST(IersFinals, RejectsMissingCpoFlagsAndNonfiniteRecords) {
    const auto time = providers::erfa::make_builtin_time_provider();
    const auto manifest = nlohmann::json::parse(read(ORBELLE_FINALS_SAMPLE));
    const auto original =
        read(std::filesystem::path(ORBELLE_FINALS_SAMPLE).parent_path() / "finals2000A.data");
    auto invalid = original;
    invalid[95] = ' ';
    EXPECT_THROW((void)providers::iers::load_eop_snapshot(fixture(invalid, manifest), *time),
                 core::EopDataError);
    invalid = original;
    invalid.replace(97, 9, "         ");
    EXPECT_THROW((void)providers::iers::load_eop_snapshot(fixture(invalid, manifest), *time),
                 core::EopDataError);
    invalid = original;
    invalid.replace(18, 9, "      nan");
    EXPECT_THROW((void)providers::iers::load_eop_snapshot(fixture(invalid, manifest), *time),
                 core::EopDataError);
}

TEST(IersFinals, PredictionProvenanceIncludesInterpolantDerivativeDependencies) {
    const auto time = providers::erfa::make_builtin_time_provider();
    const auto table = providers::iers::load_eop_snapshot(ORBELLE_FINALS_SAMPLE, *time);
    for (const auto &[date, mask] :
         {std::pair{core::UtcDateTime{2026, 9, 21, 0, 0, 0, 0}, "prediction-mask=0"},
          std::pair{core::UtcDateTime{2026, 9, 21, 12, 0, 0, 0}, "prediction-mask=4"},
          std::pair{core::UtcDateTime{2026, 9, 22, 0, 0, 0, 0}, "prediction-mask=4"},
          std::pair{core::UtcDateTime{2026, 10, 7, 0, 0, 0, 0}, "prediction-mask=4"},
          std::pair{core::UtcDateTime{2026, 10, 7, 12, 0, 0, 0}, "prediction-mask=7"}})
        EXPECT_NE(table->at(time->from_utc(date)).sample.source_id.find(mask), std::string::npos);
}

TEST(IersEop, CoverageIsClosedAndNeverExtrapolated) {
    const auto time = providers::erfa::make_builtin_time_provider();
    const auto table = load(*time);
    const auto &info = table->data_info();
    EXPECT_NO_THROW((void)table->at(info.first_epoch));
    EXPECT_NO_THROW((void)table->at(info.last_epoch));
    EXPECT_THROW((void)table->at(shift(info.first_epoch, -1e-9)), core::EopCoverageError);
    EXPECT_THROW((void)table->at(shift(info.last_epoch, 1e-9)), core::EopCoverageError);
    EXPECT_THROW((void)table->at(core::Epoch(std::numeric_limits<std::int64_t>::max())),
                 core::EopCoverageError);
}

TEST(IersEop, HermiteReproducesIndependentCubicAcrossLeapDay) {
    const auto time = providers::erfa::make_builtin_time_provider();
    auto manifest = nlohmann::json::parse(read(ORBELLE_EOP_SAMPLE));
    manifest["first_mjd"] = 57752;
    manifest["last_mjd"] = 57754;
    std::ostringstream data;
    data << "# 20 C04 sampled at 0h UTC dX(\") dY(\") synthetic cubic\n" << std::setprecision(17);
    const std::array<double, 3> seconds{0, 86400, 172801};
    for (std::size_t i = 0; i < seconds.size(); ++i) {
        const auto t = seconds[i];
        const int leap_offset = i == 2 ? 37 : 36;
        data << (i == 2   ? "2017 1 1"
                 : i == 1 ? "2016 12 31"
                          : "2016 12 30")
             << " 0 " << 57752 + i << ' ' << polynomial(t) << " 0 "
             << -36.2 - 1e-8 * t + leap_offset << " 0 0 " << polynomial_rate(t) * 86400
             << " 0 0.000864 0 0 0 0 0 0 0 0\n";
    }
    const auto table = providers::iers::load_eop_snapshot(fixture(data.str(), manifest), *time);
    for (const auto t : {1.0, 43200.0, 86400.5, 129600.25, 172800.5}) {
        const auto eop = table->at(shift(table->data_info().first_epoch, t));
        EXPECT_NEAR(eop.sample.polar_motion_x_rad, polynomial(t) * arcsec, 1e-20);
        EXPECT_NEAR(eop.rates.polar_motion_x_rad_per_second, polynomial_rate(t) * arcsec, 1e-24);
        EXPECT_NEAR(eop.sample.ut1_minus_tai_seconds, -36.2 - 1e-8 * t, 1e-12);
        EXPECT_NEAR(eop.rates.ut1_minus_tai_seconds_per_second, -1e-8, 1e-18);
    }
}

TEST(IersEop, RejectsCorruptionMissingDaysAndWrongSchema) {
    const auto time = providers::erfa::make_builtin_time_provider();
    auto manifest = nlohmann::json::parse(read(ORBELLE_EOP_SAMPLE));
    const auto data = read(std::filesystem::path(ORBELLE_EOP_SAMPLE).parent_path() / "eop.c04");
    const auto path = fixture(data, manifest);
    std::ofstream(path.parent_path() / "eop.c04", std::ios::app) << "# altered bytes\n";
    EXPECT_THROW((void)providers::iers::load_eop_snapshot(path, *time), core::EopDataError);
    auto missing = data;
    const auto begin = missing.find("2016  12  31");
    ASSERT_NE(begin, std::string::npos);
    missing.erase(begin, missing.find('\n', begin) + 1 - begin);
    EXPECT_THROW((void)providers::iers::load_eop_snapshot(fixture(missing, manifest), *time),
                 core::EopDataError);
    manifest["schema_version"] = 1.5;
    EXPECT_THROW((void)providers::iers::load_eop_snapshot(fixture(data, manifest), *time),
                 core::EopDataError);
    manifest["schema_version"] = 1;
    manifest["interpolation"] = "silent-extrapolation";
    EXPECT_THROW((void)providers::iers::load_eop_snapshot(fixture(data, manifest), *time),
                 core::EopDataError);
}

TEST(IersEop, ProvenanceDistinguishesWindowsWithDifferentBoundaryRates) {
    const auto time = providers::erfa::make_builtin_time_provider();
    const auto full = load(*time);
    auto manifest = nlohmann::json::parse(read(ORBELLE_EOP_SAMPLE));
    const auto data = read(std::filesystem::path(ORBELLE_EOP_SAMPLE).parent_path() / "eop.c04");
    manifest["first_mjd"] = 57751;
    const auto clipped = providers::iers::load_eop_snapshot(fixture(data, manifest), *time);
    const auto epoch = clipped->data_info().first_epoch;
    EXPECT_EQ(full->data_info().sha256, clipped->data_info().sha256);
    EXPECT_NE(full->at(epoch).rates.celestial_pole_dx_rad_per_second,
              clipped->at(epoch).rates.celestial_pole_dx_rad_per_second);
    EXPECT_NE(full->at(epoch).sample.source_id, clipped->at(epoch).sample.source_id);
    EXPECT_NE(clipped->at(epoch).sample.source_id.find("subdaily-tides=none"), std::string::npos);
}

TEST(IersFrames, FullVelocityMatchesPositionDerivativeAndRoundTrips) {
    const auto time = providers::erfa::make_builtin_time_provider();
    const auto table = load(*time);
    const auto frames = providers::erfa::make_frame_provider();
    const auto midnight = time->from_utc({2017, 1, 1, 0, 0, 0, 0});
    for (const auto offset : {-43200.25, -2.0, -1.0, 0.0, 1.0, 43199.5}) {
        const auto epoch = shift(midnight, offset);
        for (const auto radius : {7000000.0, 42164000.0}) {
            const core::CircularTwoBody orbit({radius}, {3.986004418e14}, epoch);
            const auto transform = frames->gcrf_to_itrf_state(epoch, table->at(epoch));
            const auto initial = orbit.state_at(epoch);
            const auto terrestrial = transform.apply(initial);
            const auto back = transform.inverse(terrestrial);
            for (std::size_t k = 0; k < 3; ++k) {
                EXPECT_NEAR(back.position_m[k], initial.position_m[k], 5e-8);
                EXPECT_NEAR(back.velocity_mps[k], initial.velocity_mps[k], 5e-10);
            }
            for (const auto h : {0.5, 1.0, 2.0}) {
                std::array<std::array<double, 3>, 4> positions{};
                const std::array offsets{-2 * h, -h, h, 2 * h};
                for (std::size_t j = 0; j < offsets.size(); ++j) {
                    const auto t = shift(epoch, offsets[j]);
                    positions[j] = frames->gcrf_to_itrf(t, table->at(t).sample)
                                       .apply_position(orbit.state_at(t).position_m);
                }
                for (std::size_t k = 0; k < 3; ++k) {
                    const double derivative = (8 * (positions[2][k] - positions[1][k]) -
                                               (positions[3][k] - positions[0][k])) /
                                              (12 * h);
                    EXPECT_NEAR(terrestrial.velocity_mps[k], derivative, 2e-5);
                }
            }
            for (std::size_t i = 0; i < 3; ++i) {
                for (std::size_t j = 0; j < 3; ++j) {
                    double symmetric = 0;
                    for (std::size_t k = 0; k < 3; ++k) {
                        symmetric += transform.rotation_rate_per_second[i][k] *
                                         transform.rotation.matrix[j][k] +
                                     transform.rotation.matrix[i][k] *
                                         transform.rotation_rate_per_second[j][k];
                    }
                    EXPECT_NEAR(symmetric, 0.0, 1e-14);
                }
            }
        }
    }
}

TEST(IersFrames, FixedGroundPointRotatesEastwardAndBadStatesAreRejected) {
    const auto time = providers::erfa::make_builtin_time_provider();
    const auto table = load(*time);
    const auto frames = providers::erfa::make_frame_provider();
    const auto epoch = time->from_utc({2017, 1, 1, 0, 0, 0, 0});
    const auto transform = frames->gcrf_to_itrf_state(epoch, table->at(epoch));
    const core::CartesianState ground{
        epoch, core::OriginId::earth, core::FrameId::itrf, {6378137, 0, 0}, {0, 0, 0}};
    const auto inertial = transform.inverse(ground);
    EXPECT_NEAR(
        std::hypot(inertial.velocity_mps[0], inertial.velocity_mps[1], inertial.velocity_mps[2]),
        465.1011, 0.001);
    EXPECT_GT(inertial.position_m[0] * inertial.velocity_mps[1] -
                  inertial.position_m[1] * inertial.velocity_mps[0],
              0);
    EXPECT_THROW((void)transform.apply(ground), std::invalid_argument);
    auto bad = inertial;
    bad.epoch = shift(epoch, 1e-9);
    EXPECT_THROW((void)transform.apply(bad), std::invalid_argument);
    bad = inertial;
    bad.origin = static_cast<core::OriginId>(999);
    EXPECT_THROW((void)transform.apply(bad), std::invalid_argument);
    bad = inertial;
    bad.velocity_mps[0] = std::numeric_limits<double>::infinity();
    EXPECT_THROW((void)transform.apply(bad), std::invalid_argument);
    auto eop = table->at(epoch);
    eop.rates.celestial_pole_dx_rad_per_second = std::numeric_limits<double>::quiet_NaN();
    EXPECT_THROW((void)frames->gcrf_to_itrf_state(epoch, eop), std::invalid_argument);
}
