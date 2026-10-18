#include "iers1996_nutation.hpp"
#include "leap_seconds.hpp"
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <erfa.h>
#include <fstream>
#include <locale>
#include <numbers>
#include <orbitforge/providers/erfa/provider.hpp>
#include <picosha2.h>
#include <sstream>
#include <stdexcept>
#include <vector>

namespace orbitforge::providers::erfa {
core::GeodeticPosition itrf_to_wgs84(core::CartesianState state) {
    auto &p = state.position_m;
    if (state.origin != core::OriginId::earth || state.frame != core::FrameId::itrf ||
        !std::all_of(p.begin(), p.end(), [](double v) { return std::isfinite(v); }) ||
        std::hypot(p[0], p[1], p[2]) < 1)
        throw std::invalid_argument(
            "WGS84 conversion requires a finite Earth-centred ITRF position away from the origin");
    core::GeodeticPosition result{};
    if (eraGc2gd(1, p.data(), &result.longitude_rad, &result.latitude_rad, &result.height_m) != 0)
        throw std::runtime_error("ERFA WGS84 conversion failed");
    return result;
}
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

std::int64_t floor_div(std::int64_t value, std::int64_t divisor) {
    const auto quotient = value / divisor;
    return quotient - (value % divisor < 0 ? 1 : 0);
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
    TimeProvider()
        : rows_(detail::leap_seconds.begin(), detail::leap_seconds.end()),
          info_(detail::time_data_info) {}
    TimeProvider(std::vector<detail::LeapSecond> rows, core::TimeDataInfo info)
        : rows_(std::move(rows)), info_(std::move(info)) {}
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
        if (mjd < info_.utc_valid_from_mjd || mjd >= info_.utc_valid_until_mjd) {
            throw core::TimeDataRangeError(
                "UTC is outside the pinned leap-second snapshot validity");
        }
        const auto day = static_cast<int>(mjd);
        if (utc.second == 60 && (utc.hour != 23 || utc.minute != 59 ||
                                 offset_at_day(day + 1) != offset_at_day(day) + 1)) {
            throw std::invalid_argument("UTC second 60 is not a leap second in this snapshot");
        }
        const auto elapsed_seconds = utc.hour * 3600 + utc.minute * 60 + utc.second;
        if (elapsed_seconds >= seconds_per_day + offset_at_day(day + 1) - offset_at_day(day))
            throw std::invalid_argument("UTC second is removed by a negative leap second");
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
    const core::TimeDataInfo &data_info() const noexcept override { return info_; }

  private:
    int offset_at_day(int mjd) const {
        const auto it = std::upper_bound(rows_.begin(), rows_.end(), mjd,
                                         [](int day, auto row) { return day < row.mjd; });
        return it == rows_.begin() ? rows_.front().tai_minus_utc : (it - 1)->tai_minus_utc;
    }
    std::int64_t day_start_tai(int mjd) const {
        return ((static_cast<std::int64_t>(mjd) - epoch_mjd) * seconds_per_day - 43'200 +
                offset_at_day(mjd)) *
               ns_per_second;
    }
    int utc_day(core::Epoch epoch) const {
        const auto ticks = epoch.tai_nanoseconds();
        if (ticks < day_start_tai(info_.utc_valid_from_mjd) ||
            ticks >= day_start_tai(info_.utc_valid_until_mjd))
            throw core::TimeDataRangeError("Epoch is outside the UTC leap-second data validity");
        auto day =
            static_cast<int>(floor_div(ticks + 43'200 * ns_per_second, ns_per_day)) + epoch_mjd;
        while (ticks < day_start_tai(day))
            --day;
        while (ticks >= day_start_tai(day + 1))
            ++day;
        return day;
    }
    std::vector<detail::LeapSecond> rows_;
    core::TimeDataInfo info_;
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

constexpr double arcsec = std::numbers::pi / (180 * 3600);
constexpr std::int64_t c7_start = -89726370000000000;
core::Matrix3 transpose(const core::Matrix3 &a) {
    core::Matrix3 result{};
    for (std::size_t i = 0; i < 3; ++i)
        for (std::size_t j = 0; j < 3; ++j)
            result[i][j] = a[j][i];
    return result;
}
core::Matrix3 spin_x(double angle) {
    const auto c = std::cos(angle), s = std::sin(angle);
    return {{{1, 0, 0}, {0, c, s}, {0, -s, c}}};
}
// All angles follow IERS 1996, including its Omega coefficient (not IERS 2003).
core::Matrix3 fk5_rotation(core::Epoch epoch, const core::EarthOrientationState &eop,
                           core::FrameId target, double dt) {
    auto tt = tt_jd(epoch);
    tt.fraction += dt / 86400;
    const double t = ((tt.day - 2451545.0) + tt.fraction) / 36525;
    constexpr std::array<std::array<double, 5>, 5> arguments{
        {{134.96340251 * 3600, 1717915923.2178, 31.8792, .051635, -.00024470},
         {357.52910918 * 3600, 129596581.0481, -.5532, .000136, -.00001149},
         {93.27209062 * 3600, 1739527262.8478, -12.7512, -.001037, .00000417},
         {297.85019547 * 3600, 1602961601.2090, -6.3706, .006593, -.00003169},
         {125.04455501 * 3600, -6962890.2665, 7.4722, .007702, -.00005939}}};
    std::array<double, 5> argument{};
    for (std::size_t i = 0; i < 5; ++i) {
        const auto &a = arguments[i];
        argument[i] =
            std::remainder(((((a[4] * t + a[3]) * t + a[2]) * t + a[1]) * t + a[0]) * arcsec,
                           2 * std::numbers::pi);
    }
    double psi = 0, eps = 0;
    // Sum the smallest terms first.
    for (auto it = detail::nutation1996.rbegin(); it != detail::nutation1996.rend(); ++it) {
        double phase = 0;
        for (std::size_t i = 0; i < 5; ++i)
            phase += it->multiplier[i] * argument[i];
        psi += (it->longitude + it->longitude_rate * t) * std::sin(phase);
        eps += (it->obliquity + it->obliquity_rate * t) * std::cos(phase);
    }
    psi *= arcsec * 1e-4;
    eps *= arcsec * 1e-4;
    const double mean = (84381.448 + (-46.8150 + (-.00059 + .001813 * t) * t) * t) * arcsec;
    double equation = psi * std::cos(mean);
    // Keep the selected side for the derivative stencil; never query outside EOP coverage.
    if (epoch.tai_nanoseconds() >= c7_start)
        equation += arcsec * (.00264 * std::sin(argument[4]) + .000063 * std::sin(2 * argument[4]));
    const auto tai = tai_jd(epoch);
    const double ut = ((tai.day - 2451545.0) + tai.fraction +
                       (eop.sample.ut1_minus_tai_seconds +
                        dt * (1 + eop.rates.ut1_minus_tai_seconds_per_second)) /
                           86400) /
                      36525;
    // GMST82 - ERA00 with the common fast Earth rotation algebraically cancelled.
    // Avoid differencing two wrapped sidereal angles when computing their tiny rate.
    const double gmst_minus_era =
        (67310.54841 - 86400 * .7790572732640 +
         ((8640184.812866 - 86400 * .00273781191135448 * 36525) + (.093104 - .0000062 * ut) * ut) *
             ut) *
        (2 * std::numbers::pi / 86400);
    const auto c = slow_rotations(tt_jd(epoch), eop, dt).celestial;
    auto rotation = multiply(spin(-gmst_minus_era - equation), c);
    if (target == core::FrameId::true_of_date)
        return rotation;
    // Lieske precession in the IERS 1996 psi_A, omega_A, chi_A representation.
    const double pa = ((-.001147 * t - 1.07259) * t + 5038.7784) * t * arcsec;
    const double oa = (84381.448 + (.05127 - .007726 * t) * t * t) * arcsec;
    const double ca = ((-.001125 * t - 2.38064) * t + 10.5526) * t * arcsec;
    const auto precession =
        multiply(multiply(multiply(spin(ca), spin_x(-oa)), spin(-pa)), spin_x(84381.448 * arcsec));
    double n[3][3]{};
    eraNumat(mean, psi, eps, n);
    return multiply(transpose(multiply(matrix_value(n), precession)), rotation);
}
void validate_celestial(core::Epoch epoch, const core::EarthOrientationSample &eop,
                        core::FrameId target) {
    validate_eop(epoch, eop);
    if (target != core::FrameId::fk5_j2000 && target != core::FrameId::true_of_date)
        throw std::invalid_argument("Celestial target must be FK5 J2000 or TrueOfDate");
}

class FrameProvider final : public core::IFrameProvider {
  public:
    core::PositionRotation gcrf_to_celestial(core::Epoch epoch,
                                             const core::EarthOrientationSample &eop,
                                             core::FrameId target) const override {
        validate_celestial(epoch, eop, target);
        return {epoch,
                core::OriginId::earth,
                core::FrameId::gcrf,
                target,
                fk5_rotation(epoch, {eop, {}}, target, 0),
                "erfa-2.0.1-iau2006-2000a-to-fk5-iers1996-c7-pseudo-fixed",
                eop.source_id};
    }
    core::StateTransform gcrf_to_celestial_state(core::Epoch epoch,
                                                 const core::EarthOrientationState &eop,
                                                 core::FrameId target) const override {
        validate_celestial(epoch, eop.sample, target);
        for (const auto value :
             {eop.rates.ut1_minus_tai_seconds_per_second, eop.rates.polar_motion_x_rad_per_second,
              eop.rates.polar_motion_y_rad_per_second, eop.rates.celestial_pole_dx_rad_per_second,
              eop.rates.celestial_pole_dy_rad_per_second})
            if (!std::isfinite(value) || std::abs(value) > .001)
                throw std::invalid_argument(
                    "EOP rates must be finite and at most 0.001 SI units/s");
        if (epoch.tai_nanoseconds() == c7_start)
            throw std::invalid_argument(
                "FK5 velocity is undefined at the IAU 1994 C7 model transition");
        auto rotation = gcrf_to_celestial(epoch, eop.sample, target);
        // Differentiate observed pole offsets analytically. Finite differencing
        // large CPO rates forces short stencils and loses the tiny FK5 spin rate.
        auto slow_eop = eop;
        slow_eop.rates.celestial_pole_dx_rad_per_second = 0;
        slow_eop.rates.celestial_pole_dy_rad_per_second = 0;
        constexpr double h = 32;
        const auto m2 = fk5_rotation(epoch, slow_eop, target, -2 * h),
                   m1 = fk5_rotation(epoch, slow_eop, target, -h),
                   p1 = fk5_rotation(epoch, slow_eop, target, h),
                   p2 = fk5_rotation(epoch, slow_eop, target, 2 * h);
        core::Matrix3 derivative{};
        for (std::size_t i = 0; i < 3; ++i)
            for (std::size_t j = 0; j < 3; ++j)
                derivative[i][j] = (8 * (p1[i][j] - m1[i][j]) - (p2[i][j] - m2[i][j])) / (12 * h);
        const auto tt = tt_jd(epoch);
        double x = 0, y = 0, s = 0;
        eraXys06a(tt.day, tt.fraction, &x, &y, &s);
        x += eop.sample.celestial_pole_dx_rad;
        y += eop.sample.celestial_pole_dy_rad;
        const auto dx = eop.rates.celestial_pole_dx_rad_per_second,
                   dy = eop.rates.celestial_pole_dy_rad_per_second;
        const double z = std::sqrt(1 - x * x - y * y), a = 1 / (1 + z);
        const double dz = -(x * dx + y * dy) / z, da = -a * a * dz;
        const double xy = -da * x * y - a * (dx * y + x * dy);
        const core::Matrix3 qdot{{{-da * x * x - 2 * a * x * dx, xy, -dx},
                                  {xy, -da * y * y - 2 * a * y * dy, -dy},
                                  {dx, dy, dz}}};
        const auto celestial = slow_rotations(tt, eop, 0).celestial;
        const auto factor = multiply(rotation.matrix, transpose(celestial));
        const auto cpo = multiply(factor, multiply(spin(-s), qdot));
        for (std::size_t i = 0; i < 3; ++i)
            for (std::size_t j = 0; j < 3; ++j)
                derivative[i][j] += cpo[i][j];
        // Remove roundoff normal to SO(3), preserving the full angular velocity.
        const auto tangent = multiply(derivative, transpose(rotation.matrix));
        core::Matrix3 omega{};
        for (std::size_t i = 0; i < 3; ++i)
            for (std::size_t j = 0; j < 3; ++j)
                omega[i][j] = .5 * (tangent[i][j] - tangent[j][i]);
        derivative = multiply(omega, rotation.matrix);
        return {std::move(rotation), derivative,
                "analytic-cpo-plus-fk5-slow-fd4-32s-local-tangent"};
    }

    core::PositionRotation teme_to_itrf(core::Epoch epoch,
                                        const core::EarthOrientationSample &eop) const override {
        validate_eop(epoch, eop);
        const auto tai = tai_jd(epoch);
        double day = 0, fraction = 0, polar[3][3]{};
        if (eraTaiut1(tai.day, tai.fraction, eop.ut1_minus_tai_seconds, &day, &fraction) != 0)
            throw std::runtime_error("ERFA TAI to UT1 conversion failed");
        // TEME uses equinox-based GMST82, not ERA or apparent sidereal time.
        // The pseudo Earth-fixed convention has no TIO locator s'.
        eraPom00(eop.polar_motion_x_rad, eop.polar_motion_y_rad, 0, polar);
        return {epoch,
                core::OriginId::earth,
                core::FrameId::teme,
                core::FrameId::itrf,
                multiply(matrix_value(polar), spin(eraGmst82(day, fraction))),
                "teme-gmst82-pom00-no-sp-v1",
                eop.source_id};
    }
    core::StateTransform teme_to_itrf_state(core::Epoch epoch,
                                            const core::EarthOrientationState &eop) const override {
        auto rotation = teme_to_itrf(epoch, eop.sample);
        for (double rate :
             {eop.rates.ut1_minus_tai_seconds_per_second, eop.rates.polar_motion_x_rad_per_second,
              eop.rates.polar_motion_y_rad_per_second, eop.rates.celestial_pole_dx_rad_per_second,
              eop.rates.celestial_pole_dy_rad_per_second})
            if (!std::isfinite(rate) || std::abs(rate) > 0.001)
                throw std::invalid_argument(
                    "EOP rates must be finite and at most 0.001 SI units/s");
        const auto tai = tai_jd(epoch);
        double day = 0, fraction = 0;
        eraTaiut1(tai.day, tai.fraction, eop.sample.ut1_minus_tai_seconds, &day, &fraction);
        const double t = ((day - 2451545.0) + fraction) / 36525;
        const double omega = (2 * std::numbers::pi / 86400) *
                             (3155760000.0 + 8640184.812866 + 0.186208 * t - 0.0000186 * t * t) /
                             (36525 * 86400.0) * (1 + eop.rates.ut1_minus_tai_seconds_per_second);
        const auto angle = eraGmst82(day, fraction), c = std::cos(angle), s = std::sin(angle);
        const core::Matrix3 zdot{
            {{-s * omega, c * omega, 0}, {-c * omega, -s * omega, 0}, {0, 0, 0}}};
        const auto polar = [&](double dt) {
            double matrix[3][3]{};
            eraPom00(eop.sample.polar_motion_x_rad + dt * eop.rates.polar_motion_x_rad_per_second,
                     eop.sample.polar_motion_y_rad + dt * eop.rates.polar_motion_y_rad_per_second,
                     0, matrix);
            return matrix_value(matrix);
        };
        const auto p = polar(0), m1 = polar(-1), p1 = polar(1);
        core::Matrix3 pdot{};
        for (std::size_t i = 0; i < 3; ++i)
            for (std::size_t j = 0; j < 3; ++j)
                pdot[i][j] = (p1[i][j] - m1[i][j]) / 2;
        auto derivative = multiply(pdot, spin(angle));
        const auto earth_spin = multiply(p, zdot);
        for (std::size_t i = 0; i < 3; ++i)
            for (std::size_t j = 0; j < 3; ++j)
                derivative[i][j] += earth_spin[i][j];
        return {std::move(rotation), derivative, "analytic-gmst82-plus-polar-fd2-1s"};
    }
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
std::unique_ptr<core::ITimeProvider> load_time_snapshot(const std::filesystem::path &file) {
    const auto check = [](bool ok, const char *message) {
        if (!ok)
            throw std::invalid_argument(message);
    };
    std::ifstream input(file, std::ios::binary | std::ios::ate);
    check(static_cast<bool>(input), "Cannot read IERS leap-second file");
    const auto size = input.tellg();
    check(size > 0 && size <= 65536, "Invalid IERS leap-second file size");
    std::string bytes(static_cast<std::size_t>(size), '\0');
    input.seekg(0);
    check(static_cast<bool>(input.read(bytes.data(), size)), "Incomplete leap-second file");
    const auto calendar_mjd = [&](int year, int month, int day) {
        double base = 0, mjd = 0;
        check(year >= 1972 && year <= 2200 && eraCal2jd(year, month, day, &base, &mjd) == 0,
              "Invalid date in leap-second file");
        check(mjd >= 41317 && mjd <= 100000, "Leap-second date outside supported range");
        return static_cast<int>(mjd);
    };
    constexpr std::array months{"January",   "February", "March",    "April",
                                "May",       "June",     "July",     "August",
                                "September", "October",  "November", "December"};
    std::vector<detail::LeapSecond> rows;
    int expires = 0;
    std::istringstream lines(bytes);
    std::string line;
    while (std::getline(lines, line)) {
        const auto first = line.find_first_not_of(" \t\r");
        if (first == std::string::npos)
            continue;
        if (line[first] == '#') {
            const auto marker = line.find("File expires on ");
            if (marker != std::string::npos) {
                check(expires == 0, "Duplicate leap-second expiration date");
                std::istringstream date(line.substr(marker + 16));
                int day = 0, year = 0;
                std::string month;
                check(static_cast<bool>(date >> day >> month >> year), "Missing expiration date");
                date >> std::ws;
                const auto found = std::find(months.begin(), months.end(), month);
                check(date.eof() && found != months.end(), "Malformed expiration date");
                expires = calendar_mjd(year, static_cast<int>(found - months.begin()) + 1, day);
            }
            continue;
        }
        std::istringstream row(line);
        row.imbue(std::locale::classic());
        double mjd = 0;
        int day = 0, month = 0, year = 0, offset = 0;
        check(static_cast<bool>(row >> mjd >> day >> month >> year >> offset),
              "Malformed IERS leap-second record");
        row >> std::ws;
        const auto date = calendar_mjd(year, month, day);
        check(row.eof() && mjd == date && offset >= 0 && offset <= 1000 && rows.size() < 512,
              "Invalid IERS leap-second record");
        check(rows.empty()
                  ? date == 41317 && offset == 10
                  : date > rows.back().mjd && std::abs(offset - rows.back().tai_minus_utc) == 1,
              "Leap-second table must start in 1972 and contain ordered one-second changes");
        rows.push_back({date, offset});
    }
    check(!rows.empty() && expires > rows.back().mjd, "Missing or invalid leap-second expiration");
    const auto hash = picosha2::hash256_hex_string(bytes);
    if (hash == detail::time_data_info.sha256)
        return make_builtin_time_provider();
    core::TimeDataInfo info{"iers-leap-seconds-" + hash.substr(0, 16), hash,
                            "https://hpiers.obspm.fr/iers/bul/bulc/Leap_Second.dat",
                            rows.front().mjd, expires};
    return std::make_unique<TimeProvider>(std::move(rows), std::move(info));
}
std::unique_ptr<core::IFrameProvider> make_frame_provider() {
    return std::make_unique<FrameProvider>();
}
std::array<double, 3> display_sun_direction_gcrf(core::Epoch epoch) {
    const auto tt = tt_jd(epoch);
    double heliocentric[2][3], barycentric[2][3];
    if (eraEpv00(tt.day, tt.fraction, heliocentric, barycentric) != 0)
        throw core::TimeDataRangeError("Display sunlight requires an epoch within 1900–2100");
    const double length = std::hypot(heliocentric[0][0], heliocentric[0][1], heliocentric[0][2]);
    return {-heliocentric[0][0] / length, -heliocentric[0][1] / length,
            -heliocentric[0][2] / length};
}
} // namespace orbitforge::providers::erfa
