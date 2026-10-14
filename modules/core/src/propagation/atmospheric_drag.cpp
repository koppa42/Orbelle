#include "atmosphere_models.hpp"
#include <algorithm>
#include <chrono>
#include <cmath>
#include <numbers>
#include <orbitforge/core/propagation/state_provider.hpp>

namespace orbitforge::core {
namespace {
constexpr double pi = std::numbers::pi, a = 6378137, f = 1 / 298.257223563;
constexpr double e2 = f * (2 - f), b = a * (1 - f);
void finite_vector(const std::array<double, 3> &v) {
    if (!std::all_of(v.begin(), v.end(), [](double x) { return std::isfinite(x); }))
        throw std::invalid_argument("Drag requires finite ITRF positions and velocities");
}
std::chrono::sys_days utc_day(const UtcDateTime &utc) {
    if (utc.year < 1600 || utc.year > 2600 || utc.hour < 0 || utc.hour > 23 || utc.minute < 0 ||
        utc.minute > 59 || utc.second < 0 || utc.second > 60 || utc.nanosecond < 0 ||
        utc.nanosecond >= 1000000000 || (utc.second == 60 && (utc.hour != 23 || utc.minute != 59)))
        throw std::invalid_argument("Invalid UTC atmosphere input");
    const auto date = std::chrono::year{utc.year} /
                      std::chrono::month{static_cast<unsigned>(utc.month)} /
                      std::chrono::day{static_cast<unsigned>(utc.day)};
    if (!date.ok())
        throw std::invalid_argument("Invalid UTC atmosphere date");
    return std::chrono::sys_days{date};
}
void weather(double daily, double average, double kp) {
    if (!std::isfinite(daily) || !std::isfinite(average) || daily <= 0 || average <= 0 ||
        daily > 1000 || average > 1000)
        throw std::invalid_argument("F10.7 flux must be finite and within (0,1000] sfu");
    if (!std::isfinite(kp) || kp < 0 || kp > 9)
        throw std::invalid_argument("Kp must be finite and within 0..9");
}
} // namespace
void validate_drag_options(const DragOptions &o) {
    if (o.atmosphere != AtmosphereModel::jacchia_roberts &&
        o.atmosphere != AtmosphereModel::nrlmsise00)
        throw std::invalid_argument("Unsupported atmospheric density model");
    if (o.low_altitude_model != LowAltitudeDensityModel::none &&
        o.low_altitude_model != LowAltitudeDensityModel::nrlmsise00)
        throw std::invalid_argument("Unsupported low altitude density model");
    if (!std::isfinite(o.cd) || o.cd < -100 || o.cd > 100)
        throw std::invalid_argument("Drag Cd must be finite and within -100..100");
    if (!std::isfinite(o.area_mass_ratio_m2_per_kg) || o.area_mass_ratio_m2_per_kg < 0 ||
        o.area_mass_ratio_m2_per_kg > 9999)
        throw std::invalid_argument("Drag area/mass ratio must be within 0..9999 m^2/kg");
    weather(o.f107, o.f107_average, o.kp);
    if (!std::isfinite(o.blending_range_m) || o.blending_range_m < 0 ||
        o.blending_range_m > 1000000)
        throw std::invalid_argument("Drag blending range must be within 0..1000 km");
    if (o.enabled && o.atmosphere == AtmosphereModel::jacchia_roberts &&
        o.low_altitude_model == LowAltitudeDensityModel::nrlmsise00 && o.blending_range_m > 910000)
        throw std::invalid_argument(
            "JR/NRLMSISE blending must stay within the 1000 km NRLMSISE domain");
}
DragOptions effective_drag_options(DragOptions o) {
    if (!o.enabled)
        return {};
    if (o.atmosphere == AtmosphereModel::nrlmsise00)
        o.low_altitude_model = LowAltitudeDensityModel::none;
    if (o.low_altitude_model == LowAltitudeDensityModel::none)
        o.blending_range_m = DragOptions{}.blending_range_m;
    return o;
}
double kp_to_ap(double kp) {
    weather(150, 150, kp);
    constexpr std::array table{0.,   2.,   3.,   4.,   5.,   6.,   7.,   9.,  12., 15.,
                               18.,  22.,  27.,  32.,  39.,  48.,  56.,  67., 80., 94.,
                               111., 132., 154., 179., 207., 236., 300., 400.};
    const double index = kp * 3;
    const auto lower = static_cast<std::size_t>(index);
    return lower == table.size() - 1
               ? table.back()
               : table[lower] + (index - lower) * (table[lower + 1] - table[lower]);
}
GeodeticPosition drag_geodetic_position(const std::array<double, 3> &r) {
    finite_vector(r);
    const double p = std::hypot(r[0], r[1]), radius = std::hypot(p, r[2]);
    if (!std::isfinite(radius) || radius < b / 2)
        throw PropagationError("Drag geodetic position is invalid near Earth's centre");
    const double longitude = p > 0 ? std::atan2(r[1], r[0]) : 0;
    if (p < 1e-8)
        return {longitude, std::copysign(pi / 2, r[2]), std::abs(r[2]) - b};
    double latitude = std::atan2(r[2], p * (1 - e2));
    for (unsigned i = 0; i < 15; ++i) {
        const double s = std::sin(latitude), n = a / std::sqrt(1 - e2 * s * s);
        const double next = std::atan2(r[2] + e2 * n * s, p);
        if (std::abs(next - latitude) < 2e-15) {
            latitude = next;
            break;
        }
        latitude = next;
        if (i == 14)
            throw PropagationError("Drag geodetic conversion did not converge");
    }
    const double s = std::sin(latitude), c = std::cos(latitude), n = a / std::sqrt(1 - e2 * s * s);
    const double height = p * c + r[2] * s - n * (1 - e2 * s * s);
    return {longitude, latitude, height};
}
AtmosphereEvaluation evaluate_atmosphere(AtmosphereModel model, const AtmosphereInput &input) {
    weather(input.f107, input.f107_average, input.kp);
    const auto &g = input.geodetic;
    if (!std::isfinite(g.longitude_rad) || std::abs(g.longitude_rad) > pi ||
        !std::isfinite(g.latitude_rad) || std::abs(g.latitude_rad) > pi / 2 ||
        !std::isfinite(input.solar_hour_angle_rad) || std::abs(input.solar_hour_angle_rad) > pi ||
        !std::isfinite(input.sun_declination_rad) || std::abs(input.sun_declination_rad) > pi / 2 ||
        !std::isfinite(g.height_m))
        throw std::invalid_argument("Invalid atmosphere geometry input");
    if (model != AtmosphereModel::jacchia_roberts && model != AtmosphereModel::nrlmsise00)
        throw std::invalid_argument("Unsupported atmospheric density model");
    const bool jr = model == AtmosphereModel::jacchia_roberts;
    if (g.height_m < (jr ? 90000 : 0) || g.height_m > (jr ? 2500000 : 1000000))
        throw PropagationError(jr ? "Jacchia-Roberts requires altitude 90..2500 km"
                                  : "NRLMSISE-00 requires altitude 0..1000 km");
    const auto day = utc_day(input.utc);
    const auto january = std::chrono::sys_days{std::chrono::year{input.utc.year} / 1 / 1};
    const auto doy = static_cast<int>((day - january).count()) + 1;
    const double seconds = input.utc.hour * 3600 + input.utc.minute * 60 + input.utc.second +
                           input.utc.nanosecond * 1e-9;
    const double utc_gmat_mjd =
        21544.5 + (day - std::chrono::sys_days{std::chrono::year{2000} / 1 / 1}).count() +
        seconds / 86400;
    const auto value =
        jr ? detail::jacchia_roberts(input, utc_gmat_mjd) : detail::nrlmsise00(input, doy, seconds);
    if (!std::isfinite(value.density_kg_m3) || value.density_kg_m3 < 0 ||
        !std::isfinite(value.temperature_k) || value.temperature_k <= 0 ||
        !std::isfinite(value.exospheric_temperature_k) || value.exospheric_temperature_k <= 0)
        throw PropagationError("Atmospheric density model produced invalid output");
    return value;
}
DragEvaluation evaluate_drag(const std::array<double, 3> &r, const std::array<double, 3> &v,
                             const std::array<double, 3> &sun, const UtcDateTime &utc,
                             const DragOptions &options) {
    validate_drag_options(options);
    if (!options.enabled)
        return {};
    finite_vector(v);
    finite_vector(sun);
    const double sun_distance = std::hypot(sun[0], sun[1], sun[2]);
    if (!std::isfinite(sun_distance) || sun_distance == 0)
        throw std::invalid_argument("Atmospheric density requires a finite nonzero Sun position");
    const auto g = drag_geodetic_position(r);
    const double angle = std::atan2(sun[0] * r[1] - sun[1] * r[0], sun[0] * r[0] + sun[1] * r[1]);
    AtmosphereInput input{utc,          g,
                          angle,        std::atan2(sun[2], std::hypot(sun[0], sun[1])),
                          options.f107, options.f107_average,
                          options.kp};
    double density;
    if (options.atmosphere == AtmosphereModel::jacchia_roberts &&
        options.low_altitude_model == LowAltitudeDensityModel::nrlmsise00 &&
        g.height_m < 90000 + options.blending_range_m) {
        const double lower = evaluate_atmosphere(AtmosphereModel::nrlmsise00, input).density_kg_m3;
        if (g.height_m <= 90000) {
            density = lower;
        } else {
            const double upper =
                evaluate_atmosphere(AtmosphereModel::jacchia_roberts, input).density_kg_m3;
            const double t = (g.height_m - 90000) / options.blending_range_m;
            const double weight = t * t * (3 - 2 * t);
            density = lower + weight * (upper - lower);
        }
    } else if (options.atmosphere == AtmosphereModel::jacchia_roberts &&
               options.low_altitude_model == LowAltitudeDensityModel::nrlmsise00 &&
               g.height_m < 90000) {
        density = evaluate_atmosphere(AtmosphereModel::nrlmsise00, input).density_kg_m3;
    } else {
        density = evaluate_atmosphere(options.atmosphere, input).density_kg_m3;
    }
    const double scale = -.5 * density * options.cd * options.area_mass_ratio_m2_per_kg *
                         std::hypot(v[0], v[1], v[2]);
    const std::array force{scale * v[0], scale * v[1], scale * v[2]};
    finite_vector(force);
    return {force, density, g};
}
} // namespace orbitforge::core
