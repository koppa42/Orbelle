#include <array>
#include <chrono>
#include <cmath>
#include <erfa.h>
#include <filesystem>
#include <fstream>
#include <future>
#include <gtest/gtest.h>
#include <limits>
#include <orbelle/providers/erfa/provider.hpp>
#include <stdexcept>
#include <string>

using namespace orbelle;

TEST(ErfaTime, DownloadedLeapTablesValidateDatesAndOwnTheirPositiveAndNegativeTransitions) {
    struct TemporaryFile {
        std::filesystem::path path =
            std::filesystem::temp_directory_path() /
            ("orbelle-leaps-" +
             std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()) + ".dat");
        ~TemporaryFile() {
            std::error_code error;
            std::filesystem::remove(path, error);
        }
        void write(const std::string &bytes) {
            std::ofstream out(path, std::ios::binary);
            out << bytes;
        }
    } file;
    const std::string header = "# File expires on 1 January 1973\n";
    const std::string first = "41317.0 1 1 1972 10\n";
    file.write(header + first + "41499.0 1 7 1972 11\n");
    const auto positive = providers::erfa::load_time_snapshot(file.path);
    const auto leap = positive->from_utc({1972, 6, 30, 23, 59, 60, 0});
    EXPECT_EQ(positive->to_utc(leap).second, 60);
    EXPECT_DOUBLE_EQ(positive->from_utc({1972, 7, 1, 0, 0, 0, 0}).seconds_since(leap), 1);
    file.write(header + first + "41499.0 1 7 1972 9\n");
    const auto negative = providers::erfa::load_time_snapshot(file.path);
    EXPECT_THROW((void)negative->from_utc({1972, 6, 30, 23, 59, 59, 0}), std::invalid_argument);
    const auto end = negative->from_utc({1972, 6, 30, 23, 59, 58, 0});
    const auto next = negative->from_utc({1972, 7, 1, 0, 0, 0, 0});
    EXPECT_DOUBLE_EQ(next.seconds_since(end), 1);
    EXPECT_EQ(negative->to_utc(next).day, 1);
    EXPECT_EQ(negative->to_utc(next).hour, 0);
    EXPECT_EQ(positive->to_utc(leap).second, 60); // Replacing the source never mutates a provider.
    EXPECT_THROW((void)negative->from_utc({1973, 1, 1, 0, 0, 0, 0}), core::TimeDataRangeError);
    for (const auto &bytes :
         {first, header + first + "41499.0 2 7 1972 11\n", header + first + "41499.0 1 7 1972 12\n",
          header + first + first, std::string("# File expires on 31 February 1973\n") + first,
          header + header + first, header + first + "<html>network error</html>"}) {
        file.write(bytes);
        EXPECT_THROW((void)providers::erfa::load_time_snapshot(file.path), std::invalid_argument);
    }
}

TEST(ErfaDisplaySun, MatchesPublishedEpv00ReferenceAndRejectsUnsupportedDates) {
    // Independent epv00 reference from ERFA 2.0.1 t_erfa_c.c at MJD(TT) 53411.52501161.
    const auto epoch = core::Epoch(static_cast<std::int64_t>(
        std::llround(((53411.52501161 - 51544.5) * 86400 - 32.184) * 1e9)));
    const auto direction = providers::erfa::display_sun_direction_gcrf(epoch);
    const std::array<double, 3> earth{-0.7757238809297706813, 0.5598052241363340596,
                                      0.2426998466481686993};
    const auto length = std::hypot(earth[0], earth[1], earth[2]);
    for (std::size_t i = 0; i < 3; ++i)
        EXPECT_NEAR(direction[i], -earth[i] / length, 2e-12);
    EXPECT_NEAR(std::hypot(direction[0], direction[1], direction[2]), 1, 2e-16);
    EXPECT_THROW(
        (void)providers::erfa::display_sun_direction_gcrf(core::Epoch(7'000'000'000'000'000'000LL)),
        core::TimeDataRangeError);
}

TEST(ErfaTime, TaiReferenceAndJ2000TtAreDistinct) {
    const auto time = providers::erfa::make_builtin_time_provider();
    EXPECT_EQ(time->from_utc({2000, 1, 1, 11, 59, 28, 0}), core::Epoch(0));
    const auto j2000 = time->from_utc({2000, 1, 1, 11, 58, 55, 816000000});
    EXPECT_EQ(j2000.tai_nanoseconds(), -32'184'000'000LL);
    const auto tt = time->julian_date(j2000, core::TimeScale::tt);
    EXPECT_EQ(tt.scale, core::TimeScale::tt);
    EXPECT_NEAR((tt.day - 2451544.5) + tt.fraction, 0.5, 2e-16);
}

TEST(ErfaTime, LeapSecondIsAnExactContinuousTaiSecond) {
    const auto time = providers::erfa::make_builtin_time_provider();
    const std::array<core::UtcDateTime, 3> dates{
        core::UtcDateTime{2016, 12, 31, 23, 59, 59, 123456789},
        core::UtcDateTime{2016, 12, 31, 23, 59, 60, 123456789},
        core::UtcDateTime{2017, 1, 1, 0, 0, 0, 123456789}};
    for (std::size_t i = 0; i < dates.size(); ++i) {
        const auto epoch = time->from_utc(dates[i]);
        EXPECT_EQ(time->to_utc(epoch), dates[i]);
        EXPECT_EQ(time->tai_minus_utc_seconds(epoch), i == 2 ? 37 : 36);
        if (i > 0) {
            EXPECT_EQ(epoch.tai_nanoseconds() - time->from_utc(dates[i - 1]).tai_nanoseconds(),
                      1'000'000'000LL);
        }
    }
    const auto midnight = time->from_utc({2017, 1, 1, 0, 0, 0, 0});
    EXPECT_EQ(time->to_utc(core::Epoch(midnight.tai_nanoseconds() - 1)),
              (core::UtcDateTime{2016, 12, 31, 23, 59, 60, 999999999}));
}

TEST(ErfaTime, EveryPublishedLeapBoundaryRoundTripsAtNanosecondResolution) {
    const auto time = providers::erfa::make_builtin_time_provider();
    // Effective dates from IERS Bulletin C 72, independent of the generated C++ table.
    const std::array<std::array<int, 3>, 27> dates{
        {{1972, 7, 1}, {1973, 1, 1}, {1974, 1, 1}, {1975, 1, 1}, {1976, 1, 1}, {1977, 1, 1},
         {1978, 1, 1}, {1979, 1, 1}, {1980, 1, 1}, {1981, 7, 1}, {1982, 7, 1}, {1983, 7, 1},
         {1985, 7, 1}, {1988, 1, 1}, {1990, 1, 1}, {1991, 1, 1}, {1992, 7, 1}, {1993, 7, 1},
         {1994, 7, 1}, {1996, 1, 1}, {1997, 7, 1}, {1999, 1, 1}, {2006, 1, 1}, {2009, 1, 1},
         {2012, 7, 1}, {2015, 7, 1}, {2017, 1, 1}}};
    for (std::size_t i = 0; i < dates.size(); ++i) {
        const auto &d = dates[i];
        const auto midnight = time->from_utc({d[0], d[1], d[2], 0, 0, 0, 0});
        EXPECT_EQ(time->tai_minus_utc_seconds(midnight), static_cast<int>(i) + 11);
        for (const std::int64_t delta : {-2'000'000'001LL, -2'000'000'000LL, -1'000'000'001LL,
                                         -1'000'000'000LL, -1LL, 0LL, 1LL, 1'000'000'000LL}) {
            const core::Epoch epoch(midnight.tai_nanoseconds() + delta);
            EXPECT_EQ(time->from_utc(time->to_utc(epoch)), epoch);
        }
        const auto last = time->to_utc(core::Epoch(midnight.tai_nanoseconds() - 1));
        EXPECT_EQ(last.hour, 23);
        EXPECT_EQ(last.minute, 59);
        EXPECT_EQ(last.second, 60);
        EXPECT_EQ(last.nanosecond, 999999999);
    }
}

TEST(ErfaTime, CalendarValidationAndSnapshotExpiryAreStrict) {
    const auto time = providers::erfa::make_builtin_time_provider();
    for (const auto utc : {core::UtcDateTime{2026, 2, 29, 0, 0, 0, 0},
                           {2026, 1, 1, 24, 0, 0, 0},
                           {2016, 12, 31, 12, 0, 60, 0},
                           {2017, 1, 1, 23, 59, 60, 0},
                           {2026, 1, 1, 0, 0, 0, -1},
                           {2026, 1, 1, 0, 0, 0, 1000000000}}) {
        EXPECT_THROW((void)time->from_utc(utc), std::invalid_argument);
    }
    EXPECT_THROW((void)time->from_utc({1971, 12, 31, 23, 59, 59, 0}), std::out_of_range);
    EXPECT_THROW((void)time->from_utc({2027, 6, 28, 0, 0, 0, 0}), std::out_of_range);
    EXPECT_THROW((void)time->from_utc({2000, 1, std::numeric_limits<int>::max(), 0, 0, 0, 0}),
                 std::invalid_argument);
    EXPECT_THROW((void)time->from_utc({2000, 1, std::numeric_limits<int>::min(), 0, 0, 0, 0}),
                 std::invalid_argument);
    EXPECT_THROW((void)time->from_utc({std::numeric_limits<int>::max(), 1, 1, 0, 0, 0, 0}),
                 core::TimeDataRangeError);
    EXPECT_THROW((void)time->to_utc(core::Epoch(std::numeric_limits<std::int64_t>::min())),
                 core::TimeDataRangeError);
    EXPECT_THROW((void)time->to_utc(core::Epoch(std::numeric_limits<std::int64_t>::max())),
                 core::TimeDataRangeError);
    const auto first = time->from_utc({1972, 1, 1, 0, 0, 0, 0});
    EXPECT_EQ(time->to_utc(first), (core::UtcDateTime{1972, 1, 1, 0, 0, 0, 0}));
    EXPECT_THROW((void)time->to_utc(core::Epoch(first.tai_nanoseconds() - 1)), std::out_of_range);
    const auto last = time->from_utc({2027, 6, 27, 23, 59, 59, 999999999});
    EXPECT_THROW((void)time->to_utc(core::Epoch(last.tai_nanoseconds() + 1)), std::out_of_range);
    EXPECT_EQ(time->data_info().id, "iers-bulletin-c-72-2026-07");
    EXPECT_EQ(time->data_info().sha256,
              "6cb6f5d4b819f2e568e25db4b0b26d89dedf031fdffb18bc94d40f4e94e268d7");
}

TEST(ErfaTime, TaiAndTtDoNotRequireUtcCoverage) {
    const auto time = providers::erfa::make_builtin_time_provider();
    for (const auto ticks : std::array<std::int64_t, 6>{
             std::numeric_limits<std::int64_t>::min(), -43'200'000'000'001LL, -1LL, 0LL,
             43'200'000'000'000LL, std::numeric_limits<std::int64_t>::max()}) {
        const core::Epoch epoch(ticks);
        const auto tai = time->julian_date(epoch, core::TimeScale::tai);
        const auto tt = time->julian_date(epoch, core::TimeScale::tt);
        EXPECT_GE(tai.fraction, 0.0);
        EXPECT_LT(tai.fraction, 1.0);
        EXPECT_GE(tt.fraction, 0.0);
        EXPECT_LT(tt.fraction, 1.0);
        EXPECT_NEAR(((tt.day - tai.day) + (tt.fraction - tai.fraction)) * 86400.0, 32.184, 2e-11);
    }
}

TEST(ErfaFrames, MatchesUpstreamC2t06aReference) {
    const auto frames = providers::erfa::make_frame_provider();
    // TT and UT1 both equal MJD 53736; TAI precedes TT by exactly 32.184 seconds.
    const core::Epoch epoch((2192LL * 86400 - 43200) * 1'000'000'000 - 32'184'000'000);
    const auto rotation = frames->gcrf_to_itrf(
        epoch, {epoch, 32.184, 2.55060238e-7, 1.860359247e-6, 0, 0, "ERFA-2.0.1-t_c2t06a"});
    std::ifstream input(std::string(ORBELLE_REFERENCE_DIR) + "/erfa_2_0_1_c2t06a.csv");
    ASSERT_TRUE(input.is_open());
    for (std::size_t row = 0; row < 3; ++row) {
        for (std::size_t col = 0; col < 3; ++col) {
            double expected = 0.0;
            ASSERT_TRUE(static_cast<bool>(input >> expected));
            EXPECT_NEAR(rotation.matrix[row][col], expected, 1e-12);
            if (col < 2) {
                char separator = 0;
                input >> separator;
                ASSERT_EQ(separator, ',');
            }
        }
    }
    EXPECT_EQ(rotation.from, core::FrameId::gcrf);
    EXPECT_EQ(rotation.to, core::FrameId::itrf);
    EXPECT_EQ(rotation.origin, core::OriginId::earth);
}

TEST(ErfaFrames, CachedSeriesMatchesDirectErfaAcrossEpochsCorrectionsAndThreads) {
    const auto time = providers::erfa::make_builtin_time_provider();
    const auto frames = providers::erfa::make_frame_provider();
    const auto check = [&](int worker) {
        double maximum_error = 0;
        for (int i = 0; i < 192; ++i) {
            // Repeated epochs, cache eviction, adjacent nanoseconds and different EOP.
            const core::Epoch epoch{(i % 96 - 48) * 1'000'000'000'000'000LL + worker};
            const core::EarthOrientationSample eop{
                epoch, -32.1 + worker * .01, (worker + 1) * 1e-6, -2e-6, (i / 96 + worker) * 1e-8,
                -2e-8, "comparison"};
            const auto tt = time->julian_date(epoch, core::TimeScale::tt);
            const auto tai = time->julian_date(epoch, core::TimeScale::tai);
            double x, y, s, day, fraction, celestial[3][3], polar[3][3], expected[3][3];
            eraXys06a(tt.day, tt.fraction, &x, &y, &s);
            eraC2ixys(x + eop.celestial_pole_dx_rad, y + eop.celestial_pole_dy_rad, s, celestial);
            eraPom00(eop.polar_motion_x_rad, eop.polar_motion_y_rad, eraSp00(tt.day, tt.fraction),
                     polar);
            eraTaiut1(tai.day, tai.fraction, eop.ut1_minus_tai_seconds, &day, &fraction);
            eraC2tcio(celestial, eraEra00(day, fraction), polar, expected);
            const auto actual = frames->gcrf_to_itrf(epoch, eop);
            for (std::size_t r = 0; r < 3; ++r)
                for (std::size_t c = 0; c < 3; ++c)
                    maximum_error =
                        std::max(maximum_error, std::abs(actual.matrix[r][c] - expected[r][c]));
            const core::EarthOrientationState state{eop, {1e-8, 1e-12, -1e-12, 1e-13, -1e-13}};
            const auto first = frames->gcrf_to_itrf_state(epoch, state);
            auto changed = state;
            changed.sample.celestial_pole_dx_rad += 1e-6;
            changed.rates.polar_motion_y_rad_per_second *= 2;
            (void)frames->gcrf_to_itrf_state(epoch, changed);
            const auto repeated = frames->gcrf_to_itrf_state(epoch, state);
            if (first.rotation.matrix != repeated.rotation.matrix ||
                first.rotation_rate_per_second != repeated.rotation_rate_per_second)
                return 1.0;
        }
        return maximum_error;
    };
    std::vector<std::future<double>> workers;
    for (int i = 0; i < 4; ++i)
        workers.push_back(std::async(std::launch::async, check, i));
    for (auto &worker : workers)
        EXPECT_LE(worker.get(), 5e-16);
}

TEST(ErfaFrames, RotationPreservesGeometryAndInverts) {
    const auto frames = providers::erfa::make_frame_provider();
    const core::Epoch epoch(0);
    const auto rotation =
        frames->gcrf_to_itrf(epoch, {epoch, -31.6, 1e-6, -2e-6, 0, 0, "synthetic-EOP"});
    for (std::size_t i = 0; i < 3; ++i) {
        for (std::size_t j = 0; j < 3; ++j) {
            double dot = 0.0;
            for (std::size_t k = 0; k < 3; ++k) {
                dot += rotation.matrix[i][k] * rotation.matrix[j][k];
            }
            EXPECT_NEAR(dot, i == j ? 1.0 : 0.0, 1e-14);
        }
    }
    const auto &r = rotation.matrix;
    const double determinant = r[0][0] * (r[1][1] * r[2][2] - r[1][2] * r[2][1]) -
                               r[0][1] * (r[1][0] * r[2][2] - r[1][2] * r[2][0]) +
                               r[0][2] * (r[1][0] * r[2][1] - r[1][1] * r[2][0]);
    EXPECT_NEAR(determinant, 1.0, 1e-14);
    const std::array position{7e6, -3e6, 5e6};
    const auto transformed = rotation.apply_position(position);
    const auto back = rotation.inverse_position(transformed);
    EXPECT_NEAR(std::hypot(transformed[0], transformed[1], transformed[2]),
                std::hypot(position[0], position[1], position[2]), 1e-8);
    for (std::size_t i = 0; i < 3; ++i) {
        EXPECT_NEAR(back[i], position[i], 1e-8);
    }
}

TEST(ErfaFrames, RejectsMissingOrWrongEpochEopAndNonfinitePositions) {
    const auto frames = providers::erfa::make_frame_provider();
    const core::Epoch epoch(0);
    const auto nan = std::numeric_limits<double>::quiet_NaN();
    for (const auto &eop :
         {core::EarthOrientationSample{core::Epoch(1), -32, 0, 0, 0, 0, "wrong-epoch"},
          {epoch, -32, 0, 0, 0, 0, ""},
          {epoch, nan, 0, 0, 0, 0, "test"},
          {epoch, -32, nan, 0, 0, 0, "test"},
          {epoch, -32, 0, nan, 0, 0, "test"},
          {epoch, 1e300, 0, 0, 0, 0, "test"},
          {epoch, -32, 4, 0, 0, 0, "test"},
          {epoch, -32, 0, 0, nan, 0, "test"},
          {epoch, -32, 0, 0, 0, nan, "test"}}) {
        EXPECT_THROW((void)frames->gcrf_to_itrf(epoch, eop), std::invalid_argument);
    }
    const auto rotation = frames->gcrf_to_itrf(epoch, {epoch, -32, 0, 0, 0, 0, "synthetic-EOP"});
    EXPECT_THROW((void)rotation.apply_position({nan, 0, 0}), std::invalid_argument);
}
