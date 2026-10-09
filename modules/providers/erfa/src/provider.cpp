#include "leap_seconds.hpp"
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <erfa.h>
#include <numbers>
#include <orbitforge/providers/erfa/provider.hpp>
#include <stdexcept>

namespace orbitforge::providers::erfa {
namespace {
constexpr std::int64_t ns_per_second = 1'000'000'000;
constexpr std::int64_t seconds_per_day = 86'400;
constexpr std::int64_t ns_per_day = seconds_per_day * ns_per_second;
constexpr int epoch_mjd = 51544;

// This provider deliberately supports the pinned post-1972 integer-offset table only.
// Validate the compiled data, including bounds that make all nanosecond arithmetic safe.
constexpr bool valid_table() {
    if (detail::utc_valid_from_mjd != detail::leap_seconds.front().mjd ||
        detail::utc_valid_from_mjd < 41317 || detail::utc_valid_until_mjd > 100000 ||
        detail::leap_seconds.back().mjd >= detail::utc_valid_until_mjd) {
        return false;
    }
    for (std::size_t i = 0; i < detail::leap_seconds.size(); ++i) {
        const auto row = detail::leap_seconds[i];
        if (row.tai_minus_utc < 0 || row.tai_minus_utc > 1000) {
            return false;
        }
        if (i > 0 && (row.mjd <= detail::leap_seconds[i - 1].mjd ||
                      row.tai_minus_utc != detail::leap_seconds[i - 1].tai_minus_utc + 1)) {
            return false;
        }
    }
    return true;
}
static_assert(valid_table(), "Unsupported leap-second snapshot");

int offset_at_day(int mjd) {
    const auto it = std::upper_bound(detail::leap_seconds.begin(), detail::leap_seconds.end(), mjd,
                                     [](int day, auto row) { return day < row.mjd; });
    return (it - 1)->tai_minus_utc;
}

std::int64_t day_start_tai(int mjd) {
    return ((static_cast<std::int64_t>(mjd) - epoch_mjd) * seconds_per_day - 43'200 +
            offset_at_day(mjd)) *
           ns_per_second;
}

std::int64_t floor_div(std::int64_t value, std::int64_t divisor) {
    const auto quotient = value / divisor;
    return quotient - (value % divisor < 0 ? 1 : 0);
}

int utc_day(core::Epoch epoch) {
    const auto ticks = epoch.tai_nanoseconds();
    if (ticks < day_start_tai(detail::utc_valid_from_mjd) ||
        ticks >= day_start_tai(detail::utc_valid_until_mjd)) {
        throw core::TimeDataRangeError(
            "Epoch is outside the pinned UTC leap-second snapshot validity");
    }
    // The range check above also makes the addition and narrowing safe.
    auto day = static_cast<int>(floor_div(ticks + 43'200 * ns_per_second, ns_per_day)) + epoch_mjd;
    while (ticks < day_start_tai(day)) {
        --day;
    }
    return day;
}

core::JulianDate tai_jd(core::Epoch epoch) {
    const auto ticks = epoch.tai_nanoseconds();
    const auto whole_days = floor_div(ticks, ns_per_day);
    auto remainder = ticks % ns_per_day;
    if (remainder < 0) {
        remainder += ns_per_day;
    }
    double day = 2451544.5 + static_cast<double>(whole_days);
    double fraction = 0.5 + static_cast<double>(remainder) / static_cast<double>(ns_per_day);
    if (fraction >= 1.0) {
        day += 1.0;
        fraction -= 1.0;
    }
    return {core::TimeScale::tai, day, fraction};
}

core::JulianDate tt_jd(core::Epoch epoch) {
    const auto tai = tai_jd(epoch);
    core::JulianDate tt{core::TimeScale::tt, 0.0, 0.0};
    if (eraTaitt(tai.day, tai.fraction, &tt.day, &tt.fraction) != 0) {
        throw std::runtime_error("ERFA TAI to TT conversion failed");
    }
    if (tt.fraction >= 1.0) {
        tt.day += 1.0;
        tt.fraction -= 1.0;
    }
    return tt;
}

class TimeProvider final : public core::ITimeProvider {
  public:
    core::Epoch from_utc(const core::UtcDateTime &utc) const override {
        // Guard ERFA's C calendar arithmetic against extreme integer years. The compiled
        // table's asserted MJD limits lie entirely within this broad year range.
        if (utc.year < 1972 || utc.year > 2200) {
            throw core::TimeDataRangeError("UTC year is outside the supported snapshot range");
        }
        if (utc.month < 1 || utc.month > 12 || utc.day < 1 || utc.day > 31) {
            throw std::invalid_argument("Invalid UTC month or day");
        }
        double base = 0.0;
        double mjd = 0.0;
        if (eraCal2jd(utc.year, utc.month, utc.day, &base, &mjd) != 0 || utc.hour < 0 ||
            utc.hour > 23 || utc.minute < 0 || utc.minute > 59 || utc.second < 0 ||
            utc.second > 60 || utc.nanosecond < 0 || utc.nanosecond >= ns_per_second) {
            throw std::invalid_argument("Invalid UTC calendar date or clock fields");
        }
        if (mjd < detail::utc_valid_from_mjd || mjd >= detail::utc_valid_until_mjd) {
            throw core::TimeDataRangeError(
                "UTC is outside the pinned leap-second snapshot validity");
        }
        const auto day = static_cast<int>(mjd);
        if (utc.second == 60 && (utc.hour != 23 || utc.minute != 59 ||
                                 offset_at_day(day + 1) != offset_at_day(day) + 1)) {
            throw std::invalid_argument("UTC second 60 is not a leap second in this snapshot");
        }
        const auto elapsed_seconds = utc.hour * 3600 + utc.minute * 60 + utc.second;
        return core::Epoch(day_start_tai(day) + elapsed_seconds * ns_per_second + utc.nanosecond);
    }

    core::UtcDateTime to_utc(core::Epoch epoch) const override {
        const int day = utc_day(epoch);
        const auto elapsed = epoch.tai_nanoseconds() - day_start_tai(day);
        const auto seconds = elapsed / ns_per_second;
        core::UtcDateTime utc{};
        double fraction = 0.0;
        if (eraJd2cal(2400000.5, day, &utc.year, &utc.month, &utc.day, &fraction) != 0) {
            throw std::runtime_error("ERFA Julian date to calendar conversion failed");
        }
        if (seconds == seconds_per_day) {
            utc.hour = 23;
            utc.minute = 59;
            utc.second = 60;
        } else {
            utc.hour = static_cast<int>(seconds / 3600);
            utc.minute = static_cast<int>((seconds % 3600) / 60);
            utc.second = static_cast<int>(seconds % 60);
        }
        utc.nanosecond = static_cast<int>(elapsed % ns_per_second);
        return utc;
    }

    int tai_minus_utc_seconds(core::Epoch epoch) const override {
        return offset_at_day(utc_day(epoch));
    }
    core::JulianDate julian_date(core::Epoch epoch, core::TimeScale scale) const override {
        switch (scale) {
        case core::TimeScale::tai:
            return tai_jd(epoch);
        case core::TimeScale::tt:
            return tt_jd(epoch);
        case core::TimeScale::tdb:
            break; // TDB has one default implementation: the data-bound CSPICE provider.
        }
        throw std::invalid_argument("Unsupported time scale");
    }
    const core::TimeDataInfo &data_info() const noexcept override { return detail::time_data_info; }
};

core::Matrix3 matrix_value(const double matrix[3][3]) {
    core::Matrix3 result{};
    for (std::size_t i = 0; i < 3; ++i) {
        for (std::size_t j = 0; j < 3; ++j) {
            result[i][j] = matrix[i][j];
        }
    }
    return result;
}
core::Matrix3 multiply(const core::Matrix3 &a, const core::Matrix3 &b) {
    core::Matrix3 result{};
    for (std::size_t i = 0; i < 3; ++i) {
        for (std::size_t j = 0; j < 3; ++j) {
            for (std::size_t k = 0; k < 3; ++k) {
                result[i][j] += a[i][k] * b[k][j];
            }
        }
    }
    return result;
}
struct SlowRotations {
    core::Matrix3 polar;
    core::Matrix3 celestial;
};
SlowRotations slow_rotations(core::JulianDate tt, const core::EarthOrientationState &eop,
                             double dt) {
    tt.fraction += dt / 86400.0;
    double x = 0, y = 0, s = 0;
    eraXys06a(tt.day, tt.fraction, &x, &y, &s);
    x += eop.sample.celestial_pole_dx_rad + dt * eop.rates.celestial_pole_dx_rad_per_second;
    y += eop.sample.celestial_pole_dy_rad + dt * eop.rates.celestial_pole_dy_rad_per_second;
    if (x * x + y * y >= 1) {
        throw std::invalid_argument("Corrected CIP coordinates lie outside the unit sphere");
    }
    double celestial[3][3]{}, polar[3][3]{};
    // IERS 2010 section 5.5.3: the tabulated/model s is independent of observed dX/dY.
    eraC2ixys(x, y, s, celestial);
    eraPom00(eop.sample.polar_motion_x_rad + dt * eop.rates.polar_motion_x_rad_per_second,
             eop.sample.polar_motion_y_rad + dt * eop.rates.polar_motion_y_rad_per_second,
             eraSp00(tt.day, tt.fraction), polar);
    return {matrix_value(polar), matrix_value(celestial)};
}
void validate_eop(core::Epoch epoch, const core::EarthOrientationSample &eop) {
    if (eop.epoch != epoch || eop.source_id.empty() || !std::isfinite(eop.ut1_minus_tai_seconds) ||
        !std::isfinite(eop.polar_motion_x_rad) || !std::isfinite(eop.polar_motion_y_rad) ||
        !std::isfinite(eop.celestial_pole_dx_rad) || !std::isfinite(eop.celestial_pole_dy_rad) ||
        std::abs(eop.ut1_minus_tai_seconds) >= 86400.0 ||
        std::abs(eop.polar_motion_x_rad) > std::numbers::pi / 2 ||
        std::abs(eop.polar_motion_y_rad) > std::numbers::pi / 2 ||
        std::abs(eop.celestial_pole_dx_rad) > 0.01 || std::abs(eop.celestial_pole_dy_rad) > 0.01) {
        throw std::invalid_argument(
            "EOP needs a matching epoch, source ID, finite SI values, "
            "|UT1-TAI| < 1 day, |xp|,|yp| <= pi/2 and |dX|,|dY| <= 0.01 rad");
    }
}
double earth_angle(core::Epoch epoch, const core::EarthOrientationSample &eop) {
    const auto tai = tai_jd(epoch);
    double ut1_day = 0.0;
    double ut1_fraction = 0.0;
    if (eraTaiut1(tai.day, tai.fraction, eop.ut1_minus_tai_seconds, &ut1_day, &ut1_fraction) != 0) {
        throw std::runtime_error("ERFA TAI to UT1 conversion failed");
    }
    return eraEra00(ut1_day, ut1_fraction);
}
core::Matrix3 spin(double angle) {
    const auto c = std::cos(angle), s = std::sin(angle);
    return {{{c, s, 0}, {-s, c, 0}, {0, 0, 1}}};
}
class FrameProvider final : public core::IFrameProvider {
  public:
    core::PositionRotation gcrf_to_itrf(core::Epoch epoch,
                                        const core::EarthOrientationSample &eop) const override {
        validate_eop(epoch, eop);
        const auto slow = slow_rotations(tt_jd(epoch), {eop, {}}, 0);
        return {epoch,
                core::OriginId::earth,
                core::FrameId::gcrf,
                core::FrameId::itrf,
                multiply(multiply(slow.polar, spin(earth_angle(epoch, eop))), slow.celestial),
                "erfa-2.0.1-iau2006-2000a-cpo-model-s",
                eop.source_id};
    }

    core::StateTransform gcrf_to_itrf_state(core::Epoch epoch,
                                            const core::EarthOrientationState &eop) const override {
        const auto &rates = eop.rates;
        for (const auto value :
             {rates.ut1_minus_tai_seconds_per_second, rates.polar_motion_x_rad_per_second,
              rates.polar_motion_y_rad_per_second, rates.celestial_pole_dx_rad_per_second,
              rates.celestial_pole_dy_rad_per_second}) {
            if (!std::isfinite(value) || std::abs(value) > 0.001) {
                throw std::invalid_argument(
                    "EOP rates must be finite and at most 0.001 SI units/s");
            }
        }
        auto rotation = gcrf_to_itrf(epoch, eop.sample);
        const auto tt = tt_jd(epoch);
        const auto middle = slow_rotations(tt, eop, 0);
        const auto m2 = slow_rotations(tt, eop, -2), m1 = slow_rotations(tt, eop, -1);
        const auto p1 = slow_rotations(tt, eop, 1), p2 = slow_rotations(tt, eop, 2);
        core::Matrix3 pdot{}, cdot{};
        for (std::size_t i = 0; i < 3; ++i) {
            for (std::size_t j = 0; j < 3; ++j) {
                pdot[i][j] =
                    (8 * (p1.polar[i][j] - m1.polar[i][j]) - (p2.polar[i][j] - m2.polar[i][j])) /
                    12;
                cdot[i][j] = (8 * (p1.celestial[i][j] - m1.celestial[i][j]) -
                              (p2.celestial[i][j] - m2.celestial[i][j])) /
                             12;
            }
        }
        const auto angle = earth_angle(epoch, eop.sample);
        const auto z = spin(angle);
        const double omega = 2 * std::numbers::pi * 1.00273781191135448 / 86400.0 *
                             (1 + rates.ut1_minus_tai_seconds_per_second);
        const auto c = std::cos(angle), s = std::sin(angle);
        const core::Matrix3 zdot{
            {{-s * omega, c * omega, 0}, {-c * omega, -s * omega, 0}, {0, 0, 0}}};
        auto rdot = multiply(multiply(pdot, z), middle.celestial);
        const auto second = multiply(multiply(middle.polar, zdot), middle.celestial);
        const auto third = multiply(multiply(middle.polar, z), cdot);
        for (std::size_t i = 0; i < 3; ++i) {
            for (std::size_t j = 0; j < 3; ++j) {
                rdot[i][j] += second[i][j] + third[i][j];
            }
        }
        return {std::move(rotation), rdot, "analytic-era-plus-polar-cip-fd4-1s-local-tangent"};
    }
};
} // namespace

std::unique_ptr<core::ITimeProvider> make_builtin_time_provider() {
    return std::make_unique<TimeProvider>();
}
std::unique_ptr<core::IFrameProvider> make_frame_provider() {
    return std::make_unique<FrameProvider>();
}
} // namespace orbitforge::providers::erfa
