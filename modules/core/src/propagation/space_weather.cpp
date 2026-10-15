#include <algorithm>
#include <chrono>
#include <cmath>
#include <limits>
#include <orbitforge/core/propagation/atmospheric_drag.hpp>

namespace orbitforge::core {
namespace {
using std::chrono::year, std::chrono::month, std::chrono::sys_days, std::chrono::year_month_day;
int day_number(const UtcDateTime &u) {
    const auto ymd = year{u.year} / month{static_cast<unsigned>(u.month)} /
                     std::chrono::day{static_cast<unsigned>(u.day)};
    if (u.year < 1600 || u.year > 2600 || !ymd.ok())
        throw std::invalid_argument("Invalid space weather UTC date");
    return static_cast<int>(sys_days{ymd}.time_since_epoch().count());
}
UtcDateTime utc_date(int number, int hour = 0) {
    const year_month_day d{sys_days{std::chrono::days{number}}};
    return {static_cast<int>(d.year()),
            static_cast<int>(static_cast<unsigned>(d.month())),
            static_cast<int>(static_cast<unsigned>(d.day())),
            hour,
            0,
            0,
            0};
}
Epoch lagged(Epoch at, int seconds) {
    const auto offset = static_cast<std::int64_t>(seconds) * 1000000000;
    if (at.tai_nanoseconds() < std::numeric_limits<std::int64_t>::min() + offset)
        throw SpaceWeatherCoverageError("Space weather lag exceeds the Epoch range");
    return Epoch{at.tai_nanoseconds() - offset};
}
void choices(AtmosphereModel model, GeomagneticUpdate update, GeomagneticSource source) {
    if ((model != AtmosphereModel::jacchia_roberts && model != AtmosphereModel::nrlmsise00) ||
        (update != GeomagneticUpdate::daily && update != GeomagneticUpdate::three_hourly &&
         update != GeomagneticUpdate::three_hourly_cubic_spline) ||
        (source != GeomagneticSource::ap && source != GeomagneticSource::kp))
        throw std::invalid_argument("Unsupported space weather sampling choice");
}
std::string date_text(int d) {
    const auto u = utc_date(d);
    return std::to_string(u.year) + "-" + std::to_string(u.month) + "-" + std::to_string(u.day);
}
double required(std::optional<double> value, int d, const char *field) {
    if (!value)
        throw SpaceWeatherCoverageError("Space weather missing " + std::string(field) + " on " +
                                        date_text(d) + "; import a complete weather resource");
    return *value;
}
void check_value(std::optional<double> value, double min, double max, const char *field) {
    if (value && (!std::isfinite(*value) || *value < min || *value > max))
        throw std::invalid_argument("Invalid space weather " + std::string(field));
}
struct Slot {
    int day, index;
};
Slot slot(const UtcDateTime &utc, int earlier = 0) {
    const int index = utc.hour / 3 - earlier;
    // Floor division, including negative offsets across multiple UTC midnights.
    const int delta = index >= 0 ? index / 8 : (index - 7) / 8;
    return {day_number(utc) + delta, index - 8 * delta};
}
// Left interpolation node; at exactly 20:00 the right node is not required.
std::pair<int, bool> flux_node(const UtcDateTime &u) {
    return {day_number(u) - (u.hour < 20 ? 1 : 0),
            !(u.hour == 20 && u.minute == 0 && u.second == 0 && u.nanosecond == 0)};
}
// Left midpoint node of the interpolation interval, also at an exact knot.
int spline_node(const UtcDateTime &u) {
    const auto s = slot(u);
    const int seconds = (u.hour % 3) * 3600 + u.minute * 60 + u.second;
    return s.day * 8 + s.index - (seconds < 5400 ? 1 : 0);
}
UtcDateTime spline_utc(int node) {
    const int d = node >= 0 ? node / 8 : (node - 7) / 8;
    auto utc = utc_date(d, (node - d * 8) * 3 + 1);
    utc.minute = 30;
    return utc;
}
double spline_value(const std::array<double, 4> &c, double x) {
    return ((c[3] * x + c[2]) * x + c[1]) * x + c[0];
}
double check_spline(double value, GeomagneticSource source) {
    const double upper = source == GeomagneticSource::ap ? 400 : 9;
    const double roundoff = 64 * std::numeric_limits<double>::epsilon() * upper;
    if (!std::isfinite(value) || value < -roundoff || value > upper + roundoff)
        throw SpaceWeatherCoverageError(
            "Natural cubic geomagnetic spline overshoots the Ap/Kp range; "
            "choose daily/three-hour updates or import a suitable weather resource");
    return std::clamp(value, 0.0, upper); // Only roundoff-sized excursions reach this line.
}
} // namespace
SpaceWeather::SpaceWeather(SpaceWeatherInfo info, std::vector<SpaceWeatherDay> days)
    : info_(std::move(info)), days_(std::move(days)) {
    if (days_.empty() || days_.size() > 366000)
        throw std::invalid_argument("Space weather needs 1..366000 daily records");
    for (const auto &d : days_) {
        const auto n = day_number(d.date);
        if (d.date.hour || d.date.minute || d.date.second || d.date.nanosecond ||
            (!day_numbers_.empty() && n <= day_numbers_.back()))
            throw std::invalid_argument("Space weather dates must be unique, ordered UTC days");
        day_numbers_.push_back(n);
        for (const auto v : d.kp)
            check_value(v, 0, 9, "Kp");
        for (const auto v : d.ap)
            check_value(v, 0, 400, "Ap");
        check_value(d.daily_kp, 0, 9, "daily Kp");
        check_value(d.daily_ap, 0, 400, "daily Ap");
        for (auto v : {d.f107_observed, d.f107_adjusted, d.observed_center81, d.observed_last81,
                       d.adjusted_center81, d.adjusted_last81})
            check_value(v, 1e-12, 1000, "F10.7");
        if (d.quality < WeatherQuality::observed || d.quality > WeatherQuality::monthly_prediction)
            throw std::invalid_argument("Invalid space weather flux quality");
    }
    for (std::size_t source = 0; source < 2; ++source) {
        auto &second = spline_second_[source];
        second.resize(days_.size() * 8, std::numeric_limits<double>::quiet_NaN());
        const auto value = [&](std::size_t i) {
            const auto &d = days_[i / 8];
            return d.quality == WeatherQuality::monthly_prediction
                       ? std::optional<double>{}
                       : (source == 0 ? d.ap[i % 8] : d.kp[i % 8]);
        };
        std::size_t begin = 0;
        while (begin < second.size()) {
            if (!value(begin)) {
                ++begin;
                continue;
            }
            auto end = begin + 1;
            while (end < second.size() && value(end) &&
                   (end % 8 || day_numbers_[end / 8] == day_numbers_[end / 8 - 1] + 1))
                ++end;
            // Thomas solve: M[i-1] + 4 M[i] + M[i+1] = 6(y[i+1]-2y[i]+y[i-1]).
            // Natural boundary conditions M[0]=M[n-1]=0; two nodes are linear.
            std::vector<double> upper(end - begin, 0);
            second[begin] = second[end - 1] = 0;
            for (auto i = begin + 1; i + 1 < end; ++i) {
                const double denominator = 4 - upper[i - begin - 1];
                upper[i - begin] = 1 / denominator;
                second[i] =
                    (6 * (value(i + 1).value() - 2 * value(i).value() + value(i - 1).value()) -
                     second[i - 1]) /
                    denominator;
            }
            for (auto i = end - 1; i > begin + 1; --i)
                second[i - 1] -= upper[i - begin - 1] * second[i];
            begin = end;
        }
    }
    info_.first = days_.front().date;
    info_.last = days_.back().date;
}
const SpaceWeatherDay &SpaceWeather::day(int number) const {
    const auto it = std::lower_bound(day_numbers_.begin(), day_numbers_.end(), number);
    if (it == day_numbers_.end() || *it != number)
        throw SpaceWeatherCoverageError("Space weather missing date " + date_text(number) +
                                        "; raw records cover " + date_text(day_numbers_.front()) +
                                        " through " + date_text(day_numbers_.back()) +
                                        "; import data including model lags/history");
    const auto &record = days_[static_cast<std::size_t>(it - day_numbers_.begin())];
    if (record.quality == WeatherQuality::monthly_prediction)
        throw SpaceWeatherCoverageError("Monthly predictions require a monthly weather model; "
                                        "daily/three-hour sampling is unavailable on " +
                                        date_text(number));
    return record;
}
SpaceWeather::SplineInterval SpaceWeather::spline_interval(int node, const ITimeProvider &time,
                                                           GeomagneticSource source) const {
    const auto get = [&](int n) {
        const auto u = spline_utc(n);
        const auto d = day_number(u);
        const auto &r = day(d);
        const auto index = static_cast<std::size_t>(&r - days_.data()) * 8 + u.hour / 3;
        const double y =
            required(source == GeomagneticSource::ap ? r.ap[u.hour / 3] : r.kp[u.hour / 3], d,
                     "three-hour spline node");
        return std::pair{y, spline_second_[source == GeomagneticSource::ap ? 0 : 1][index]};
    };
    const auto [y0, m0] = get(node);
    const auto [y1, m1] = get(node + 1);
    return {{{y0, y1 - y0 - (2 * m0 + m1) / 6, m0 / 2, (m1 - m0) / 6}},
            time.from_utc(spline_utc(node)),
            time.from_utc(spline_utc(node + 1))};
}
double SpaceWeather::spline_sample(Epoch at, const ITimeProvider &time,
                                   GeomagneticSource source) const {
    const auto utc = time.to_utc(at);
    const int node = spline_node(utc);
    const auto first = time.from_utc(spline_utc(node));
    if (at == first) {
        const auto &r = day(day_number(utc));
        return required(source == GeomagneticSource::ap ? r.ap[utc.hour / 3] : r.kp[utc.hour / 3],
                        day_number(utc), "three-hour spline node");
    }
    const auto s = spline_interval(node, time, source);
    const auto result =
        spline_value(s.coefficients, at.seconds_since(s.first) / s.last.seconds_since(s.first));
    return check_spline(result, source);
}
void SpaceWeather::validate_spline(Epoch first, Epoch last, const ITimeProvider &time,
                                   GeomagneticSource source) const {
    const int begin = spline_node(time.to_utc(first)), end = spline_node(time.to_utc(last));
    for (int n = begin; n <= end; ++n) {
        // A final exact knot needs no following record and may be the last available node.
        if (n == end && last == time.from_utc(spline_utc(n)))
            break;
        const auto s = spline_interval(n, time, source);
        const auto &c = s.coefficients;
        const double duration = s.last.seconds_since(s.first);
        const double a = std::max(0.0, first.seconds_since(s.first) / duration);
        const double b = std::min(1.0, last.seconds_since(s.first) / duration);
        check_spline(spline_value(c, a), source);
        check_spline(spline_value(c, b), source);
        const auto extremum = [&](double x) {
            if (x > a && x < b)
                check_spline(spline_value(c, x), source);
        };
        if (c[3] == 0) {
            if (c[2] != 0)
                extremum(-c[1] / (2 * c[2]));
        } else {
            const double discriminant = c[2] * c[2] - 3 * c[3] * c[1];
            if (discriminant >= 0) {
                // Stable quadratic roots, including c[1]==0 and double roots.
                const double q = -c[2] - std::copysign(std::sqrt(discriminant), c[2]);
                extremum(q / (3 * c[3]));
                if (q != 0)
                    extremum(c[1] / q);
            }
        }
    }
}
double ap_to_kp(double ap) {
    if (!std::isfinite(ap) || ap < 0 || ap > 400)
        throw std::invalid_argument("Ap must be finite and within 0..400");
    for (int i = 0; i < 27; ++i) {
        const double lower = kp_to_ap(i / 3.0), upper = kp_to_ap((i + 1) / 3.0);
        if (ap <= upper)
            return (i + (ap - lower) / (upper - lower)) / 3.0;
    }
    return 9;
}
double SpaceWeather::flux_value(int number, bool average, const ITimeProvider &time,
                                const std::function<double(Epoch)> &sun_distance_m) const {
    const auto &r = day(number);
    if (!info_.solar_flux_adjusted)
        return required(average ? r.observed_center81 : r.f107_observed, number,
                        average ? "centered observed F10.7" : "observed F10.7");
    const double adjusted = required(average ? r.adjusted_center81 : r.f107_adjusted, number,
                                     average ? "centered adjusted F10.7" : "adjusted F10.7");
    if (!sun_distance_m)
        throw SpaceWeatherCoverageError(
            "Adjusted FXM flux requires a geometric Sun-Earth distance provider");
    const double distance = sun_distance_m(time.from_utc(utc_date(number, 20)));
    if (!std::isfinite(distance) || distance <= 0)
        throw SpaceWeatherCoverageError(
            "FXM Sun-Earth distance must be finite and positive in metres");
    const double scale = 149597870700.0 / distance; // IAU 2012 exact astronomical unit, SI.
    const double observed = adjusted * scale * scale;
    if (!std::isfinite(observed) || observed <= 0 || observed > 1000)
        throw SpaceWeatherCoverageError(
            "Distance-corrected FXM flux is outside the density model range (0, 1000] sfu");
    return observed;
}
SpaceWeatherSample SpaceWeather::sample(Epoch at, const ITimeProvider &time, AtmosphereModel model,
                                        GeomagneticUpdate update, GeomagneticSource source,
                                        const std::function<double(Epoch)> &sun_distance_m) const {
    choices(model, update, source);
    const bool jr = model == AtmosphereModel::jacchia_roberts;
    const auto flux = [&](Epoch epoch, bool average) {
        const auto [d, interpolate] = flux_node(time.to_utc(epoch));
        const auto field = [&](int number) {
            return flux_value(number, average, time, sun_distance_m);
        };
        const double left = field(d);
        if (!interpolate)
            return left;
        const auto a = time.from_utc(utc_date(d, 20)), b = time.from_utc(utc_date(d + 1, 20));
        return left + epoch.seconds_since(a) / b.seconds_since(a) * (field(d + 1) - left);
    };
    const auto utc = time.to_utc(lagged(at, jr ? 24120 : 0));
    const int d = day_number(utc);
    const auto &record = day(d);
    const bool from_ap = source == GeomagneticSource::ap;
    const auto daily = [&] {
        return required(from_ap ? record.daily_ap : record.daily_kp, d,
                        from_ap ? "daily Ap" : "daily Kp");
    };
    const auto hourly = [&](int earlier) {
        const auto s = slot(utc, earlier);
        const auto &r = day(s.day);
        return required(from_ap ? r.ap[s.index] : r.kp[s.index], s.day,
                        from_ap ? "three-hour Ap" : "three-hour Kp");
    };
    const double geomag = update == GeomagneticUpdate::daily ? daily()
                          : jr && update == GeomagneticUpdate::three_hourly_cubic_spline
                              ? spline_sample(lagged(at, 24120), time, source)
                              : hourly(0);
    SpaceWeatherSample result{flux(lagged(at, 86400), false),
                              flux(lagged(at, jr ? 86400 : 0), true),
                              from_ap ? ap_to_kp(geomag) : geomag,
                              from_ap ? geomag : kp_to_ap(geomag),
                              {}};
    if (!jr && update != GeomagneticUpdate::daily) {
        std::array<double, 7> history{};
        const auto as_ap = [&](double value) { return from_ap ? value : kp_to_ap(value); };
        history[0] = as_ap(daily());
        for (int i = 0; i < 4; ++i)
            history[i + 1] = as_ap(hourly(i));
        for (int i = 4; i < 20; ++i)
            history[i < 12 ? 5 : 6] += as_ap(hourly(i)) / 8;
        result.ap_history = history;
    }
    return result;
}
void SpaceWeather::validate_interval(Epoch first, Epoch last, const ITimeProvider &time,
                                     AtmosphereModel model, GeomagneticUpdate update,
                                     GeomagneticSource source,
                                     const std::function<double(Epoch)> &sun_distance_m) const {
    choices(model, update, source);
    if (first.tai_nanoseconds() > last.tai_nanoseconds())
        throw std::invalid_argument("Space weather interval must be ordered");
    const bool jr = model == AtmosphereModel::jacchia_roberts;
    const auto flux = [&](int lag, bool average) {
        const auto begin = flux_node(time.to_utc(lagged(first, lag)));
        const auto end = flux_node(time.to_utc(lagged(last, lag)));
        for (int d = begin.first; d <= end.first + (end.second ? 1 : 0); ++d) {
            (void)flux_value(d, average, time, sun_distance_m);
            (void)time.from_utc(utc_date(d, 20));
        }
    };
    flux(86400, false);
    flux(jr ? 86400 : 0, true);
    const auto a = time.to_utc(lagged(first, jr ? 24120 : 0));
    const auto b = time.to_utc(lagged(last, jr ? 24120 : 0));
    const bool ap = source == GeomagneticSource::ap;
    if (update == GeomagneticUpdate::daily || !jr)
        for (int d = day_number(a); d <= day_number(b); ++d) {
            const auto &r = day(d);
            (void)required(ap ? r.daily_ap : r.daily_kp, d, ap ? "daily Ap" : "daily Kp");
        }
    if (jr && update == GeomagneticUpdate::three_hourly_cubic_spline)
        validate_spline(lagged(first, 24120), lagged(last, 24120), time, source);
    else if (update != GeomagneticUpdate::daily) {
        const auto start = slot(a, jr ? 0 : 19), stop = slot(b);
        for (int n = start.day * 8 + start.index; n <= stop.day * 8 + stop.index; ++n) {
            const int d = n >= 0 ? n / 8 : (n - 7) / 8, i = n - d * 8;
            const auto &r = day(d);
            (void)required(ap ? r.ap[i] : r.kp[i], d, ap ? "three-hour Ap" : "three-hour Kp");
        }
    }
    (void)sample(first, time, model, update, source, sun_distance_m);
    (void)sample(last, time, model, update, source, sun_distance_m);
}
} // namespace orbitforge::core
