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
        (update != GeomagneticUpdate::daily && update != GeomagneticUpdate::three_hourly) ||
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
SpaceWeatherSample SpaceWeather::sample(Epoch at, const ITimeProvider &time, AtmosphereModel model,
                                        GeomagneticUpdate update, GeomagneticSource source) const {
    choices(model, update, source);
    const bool jr = model == AtmosphereModel::jacchia_roberts;
    const auto flux = [&](Epoch epoch, bool average) {
        const auto [d, interpolate] = flux_node(time.to_utc(epoch));
        const auto field = [&](int number) {
            const auto &r = day(number);
            return required(average ? r.observed_center81 : r.f107_observed, number,
                            average ? "centered observed F10.7" : "observed F10.7");
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
    const double geomag = update == GeomagneticUpdate::daily ? daily() : hourly(0);
    SpaceWeatherSample result{flux(lagged(at, 86400), false),
                              flux(lagged(at, jr ? 86400 : 0), true),
                              from_ap ? ap_to_kp(geomag) : geomag,
                              from_ap ? geomag : kp_to_ap(geomag),
                              {}};
    if (!jr && update == GeomagneticUpdate::three_hourly) {
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
                                     GeomagneticSource source) const {
    choices(model, update, source);
    if (first.tai_nanoseconds() > last.tai_nanoseconds())
        throw std::invalid_argument("Space weather interval must be ordered");
    const bool jr = model == AtmosphereModel::jacchia_roberts;
    const auto flux = [&](int lag, bool average) {
        const auto begin = flux_node(time.to_utc(lagged(first, lag)));
        const auto end = flux_node(time.to_utc(lagged(last, lag)));
        for (int d = begin.first; d <= end.first + (end.second ? 1 : 0); ++d) {
            const auto &r = day(d);
            (void)required(average ? r.observed_center81 : r.f107_observed, d,
                           average ? "centered observed F10.7" : "observed F10.7");
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
    if (update == GeomagneticUpdate::three_hourly) {
        const auto start = slot(a, jr ? 0 : 19), stop = slot(b);
        for (int n = start.day * 8 + start.index; n <= stop.day * 8 + stop.index; ++n) {
            const int d = n >= 0 ? n / 8 : (n - 7) / 8, i = n - d * 8;
            const auto &r = day(d);
            (void)required(ap ? r.ap[i] : r.kp[i], d, ap ? "three-hour Ap" : "three-hour Kp");
        }
    }
    (void)sample(first, time, model, update, source);
    (void)sample(last, time, model, update, source);
}
} // namespace orbitforge::core
