#include "../src/cip_interpolation.hpp"
#include <algorithm>
#include <bit>
#include <cstdint>
#include <gtest/gtest.h>
#include <limits>
#include <orbelle/providers/erfa/provider.hpp>

using namespace orbelle;

namespace {
std::array<double, 3> reference(core::JulianDate tt) {
    std::array<double, 3> value;
    eraXys06a(tt.day, tt.fraction, &value[0], &value[1], &value[2]);
    return value;
}
} // namespace

TEST(CipInterpolation, FullEpochRangeAndDayBoundariesMatchTheCompleteIauSeries) {
    const auto time = providers::erfa::make_builtin_time_provider();
    std::uint64_t random = 0x79c015c18ab21e55ULL;
    double maximum_error = 0;
    for (int sample = 0; sample < 258; ++sample) {
        random = random * 6364136223846793005ULL + 1442695040888963407ULL;
        const auto ticks = sample == 0   ? std::numeric_limits<std::int64_t>::min()
                           : sample == 1 ? std::numeric_limits<std::int64_t>::max()
                                         : std::bit_cast<std::int64_t>(random);
        const auto tt = time->julian_date(core::Epoch{ticks}, core::TimeScale::tt);
        SCOPED_TRACE(ticks);
        // Dense independent interior points, the two neighbouring windows,
        // and instants one nanosecond to either side of an exact TT day.
        const double day =
            std::floor(tt.day) + std::floor((tt.day - std::floor(tt.day)) + tt.fraction);
        const auto check = [&](double seconds) {
            const core::JulianDate at{core::TimeScale::tt, day, seconds / 86400};
            const auto expected = reference(at);
            const auto actual = providers::erfa::detail::interpolated_cip(at);
            for (std::size_t axis = 0; axis < expected.size(); ++axis)
                maximum_error = std::max(maximum_error, std::abs(actual[axis] - expected[axis]));
        };
        for (const double seconds :
             {-2., -1., -1e-9, 0., 1e-9, 1., 2., 137.125, 371.25, 811.5, 1439.75, 1913.25, 2357.5,
              2971.125, 3598., 3599., 3600., 3601., 3602.}) {
            check(seconds);
            check(86400 - seconds);
        }
        for (int i = 0; i < 97; ++i)
            check((i + .371) * 86400 / 97);
    }
    RecordProperty("maximum_component_error_rad", maximum_error);
    // 5e-16 rad is about 0.1 nanoarcseconds, below 4 nm at a 7000 km radius.
    // Keep the existing frame/independent-orbit tolerances unchanged as well.
    EXPECT_LE(maximum_error, 5e-16);
}

TEST(CipInterpolation, VelocityStencilsRemainAccurateAcrossDayBoundariesAndCacheEviction) {
    double maximum_rate_error = 0;
    for (const double day : {2345000.5, 2440587.5, 2451544.5, 2457754.5, 2461337.5, 2558000.5}) {
        for (const double seconds : {-2., -1., -1e-9, 0., 1e-9, 1., 2.}) {
            const core::JulianDate at{core::TimeScale::tt, day, .5 + seconds / 86400};
            std::array<std::array<double, 3>, 4> exact, fast;
            constexpr std::array shifts{-2., -1., 1., 2.};
            for (std::size_t i = 0; i < shifts.size(); ++i) {
                auto shifted = at;
                shifted.fraction += shifts[i] / 86400;
                exact[i] = reference(shifted);
                fast[i] = providers::erfa::detail::interpolated_cip(shifted);
            }
            for (std::size_t axis = 0; axis < 3; ++axis) {
                const double expected =
                    (8 * (exact[2][axis] - exact[1][axis]) - (exact[3][axis] - exact[0][axis])) /
                    12;
                const double actual =
                    (8 * (fast[2][axis] - fast[1][axis]) - (fast[3][axis] - fast[0][axis])) / 12;
                maximum_rate_error = std::max(maximum_rate_error, std::abs(actual - expected));
            }
            const auto before = providers::erfa::detail::interpolated_cip(at);
            for (int other = 1; other <= 8; ++other)
                (void)providers::erfa::detail::interpolated_cip(
                    {core::TimeScale::tt, day + other, .123456789});
            EXPECT_EQ(before, providers::erfa::detail::interpolated_cip(at));
        }
    }
    RecordProperty("maximum_rate_error_rad_per_second", maximum_rate_error);
    EXPECT_LE(maximum_rate_error, 3e-16);
}
