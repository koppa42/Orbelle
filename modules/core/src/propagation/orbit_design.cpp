#include <algorithm>
#include <cmath>
#include <numbers>
#include <orbelle/core/propagation/central_body.hpp>
#include <orbelle/core/propagation/orbit_design.hpp>

namespace orbelle::core {
namespace {
constexpr double pi = std::numbers::pi, tau = 2 * pi;
constexpr double maximum_axis = 106'378'136.6; // Explicit preliminary-design domain.
void require(bool valid, OrbitDesignErrorCode code, const char *message) {
    if (!valid)
        throw OrbitDesignError(code, message);
}
void range(double value, double minimum, double maximum) {
    require(std::isfinite(value) && value >= minimum && value <= maximum,
            OrbitDesignErrorCode::invalid_parameter, "Orbit design parameter is out of range");
}
double angle(double value) {
    require(std::isfinite(value), OrbitDesignErrorCode::invalid_parameter,
            "Orbit design angle must be finite");
    double result = std::fmod(value, tau);
    return result < 0 ? result + tau : result;
}
struct Rates {
    double node, apsis, mean;
};
Rates rates(double a, double e, double i) {
    const auto &earth = central_body(OriginId::earth);
    const double n = std::sqrt(earth.mu.metres_cubed_per_second_squared / a) / a;
    const double beta2 = (1 - e) * (1 + e), p = a * beta2, c = std::cos(i);
    const double factor = earth.j2 * std::pow(earth.equatorial_radius_m / p, 2) * n;
    // Same first-order secular convention as the J2 propagator; no J2^2 terms.
    return {-1.5 * factor * c, 0.75 * factor * (5 * c * c - 1),
            n + 0.75 * factor * std::sqrt(beta2) * (3 * c * c - 1)};
}
template <class Function> double solve(double lower, double upper, Function residual) {
    double left = residual(lower), right = residual(upper);
    require(std::isfinite(left) && std::isfinite(right) &&
                (left == 0 || right == 0 || std::signbit(left) != std::signbit(right)),
            OrbitDesignErrorCode::no_solution, "No orbit satisfies these design constraints");
    if (left == 0)
        return lower;
    if (right == 0)
        return upper;
    for (int iteration = 0; iteration < 100; ++iteration) {
        const double middle = lower + (upper - lower) / 2;
        if (middle == lower || middle == upper)
            return middle;
        const double value = residual(middle);
        require(std::isfinite(value), OrbitDesignErrorCode::no_solution,
                "Nonfinite orbit design residual");
        if (value == 0)
            return middle;
        if (std::signbit(value) == std::signbit(left)) {
            lower = middle;
            left = value;
        } else {
            upper = middle;
        }
    }
    return lower + (upper - lower) / 2;
}
double sun_inclination(double a) {
    const double cosine = -design_sun_rate_rad_s / (-rates(a, 0, 0).node);
    require(cosine >= -1 && cosine < 0, OrbitDesignErrorCode::no_solution,
            "Sun synchronous inclination is unavailable at this altitude");
    return std::acos(cosine);
}
} // namespace
EarthOrbitDesign design_earth_orbit(const EarthOrbitDesignRequest &q) {
    const auto &earth = central_body(OriginId::earth);
    const double radius = earth.equatorial_radius_m;
    auto height = [](double value) {
        range(value, 0, 100'000'000);
        return value;
    };
    double a = 0, e = 0, i = 0, omega = 0;
    bool j2 = true;
    const double critical = std::acos(1 / std::sqrt(5.0));
    const double sun_max_axis =
        std::pow(1.5 * earth.j2 * std::sqrt(earth.mu.metres_cubed_per_second_squared) * radius *
                     radius / design_sun_rate_rad_s,
                 2.0 / 7);
    switch (q.type) {
    case EarthOrbitType::circular:
        a = radius + height(q.altitude_m);
        i = q.inclination_rad;
        j2 = false;
        break;
    case EarthOrbitType::orbit_designer:
        a = q.semi_major_axis_m;
        e = q.eccentricity;
        i = q.inclination_rad;
        omega = angle(q.argument_of_periapsis_rad);
        j2 = false;
        break;
    case EarthOrbitType::critically_inclined: {
        const double rp = radius + height(q.perigee_altitude_m);
        const double ra = radius + height(q.apogee_altitude_m);
        require(ra >= rp, OrbitDesignErrorCode::invalid_parameter,
                "Apogee must not be below perigee");
        a = (rp + ra) / 2;
        e = (ra - rp) / (ra + rp);
        i = q.retrograde ? pi - critical : critical;
        omega = angle(q.argument_of_periapsis_rad);
        break;
    }
    case EarthOrbitType::critically_inclined_sun_sync: {
        const double rp = radius + height(q.perigee_altitude_m);
        i = pi - critical;
        a = solve(rp, maximum_axis, [&](double axis) {
            return rates(axis, 1 - rp / axis, i).node - design_sun_rate_rad_s;
        });
        e = 1 - rp / a;
        omega = angle(q.argument_of_periapsis_rad);
        break;
    }
    case EarthOrbitType::geosynchronous:
        // Ideal synchronous period. Inclined orbits describe an analemma; only
        // the prograde equatorial circular case is stationary in uniform rotation.
        a = std::cbrt(earth.mu.metres_cubed_per_second_squared /
                      (design_earth_rotation_rad_s * design_earth_rotation_rad_s));
        i = q.inclination_rad;
        require(std::isfinite(i) && i >= 0 && i < pi / 2, OrbitDesignErrorCode::invalid_parameter,
                "Geosynchronous design requires a prograde inclination below 90 degrees");
        j2 = false;
        break;
    case EarthOrbitType::molniya: {
        const double rp = radius + height(q.perigee_altitude_m);
        i = critical;
        a = solve(rp, maximum_axis, [&](double axis) {
            return rates(axis, 1 - rp / axis, i).mean - 2 * design_earth_rotation_rad_s;
        });
        e = 1 - rp / a;
        omega = angle(q.argument_of_periapsis_rad);
        break;
    }
    case EarthOrbitType::sun_synchronous:
        if (q.sun_sync_by_inclination) {
            range(q.inclination_rad, pi / 2, pi);
            i = q.inclination_rad;
            require(i > pi / 2, OrbitDesignErrorCode::no_solution,
                    "Sun synchronous inclination must be retrograde");
            a = sun_max_axis * std::pow(-std::cos(i), 2.0 / 7);
        } else {
            a = radius + height(q.altitude_m);
            i = sun_inclination(a);
        }
        break;
    case EarthOrbitType::repeating_ground_trace:
    case EarthOrbitType::repeating_sun_sync: {
        require(q.repeat_revolutions > 0 && q.repeat_revolutions <= 1'000'000 &&
                    q.repeat_days > 0 && q.repeat_days <= 1'000'000,
                OrbitDesignErrorCode::repeat_ratio,
                "Repeat revolutions and days must be positive integers");
        const bool sun = q.type == EarthOrbitType::repeating_sun_sync;
        if (!sun)
            range(q.inclination_rad, 0, pi);
        // Near the retrograde equatorial limit numerical roundoff in acos must
        // not turn an otherwise valid bracket into an out-of-domain evaluation.
        const double upper = sun ? sun_max_axis * (1 - 1e-14) : maximum_axis;
        a = solve(radius + 1e-5, upper, [&](double axis) {
            const auto r = rates(axis, 0, sun ? sun_inclination(axis) : q.inclination_rad);
            return (r.mean + r.apsis) / (design_earth_rotation_rad_s - r.node) -
                   static_cast<double>(q.repeat_revolutions) / q.repeat_days;
        });
        i = sun ? sun_inclination(a) : q.inclination_rad;
        break;
    }
    default:
        throw OrbitDesignError(OrbitDesignErrorCode::invalid_parameter,
                               "Unknown orbit design type");
    }
    range(a, radius, maximum_axis);
    range(e, 0, 0.99999);
    range(i, 0, pi);
    require(a * (1 - e) > radius, OrbitDesignErrorCode::below_surface,
            "Perigee must be above the Earth reference surface");
    const auto r = j2 ? rates(a, e, i)
                      : Rates{0, 0, std::sqrt(earth.mu.metres_cubed_per_second_squared / a) / a};
    const double period = tau / (r.mean + r.apsis);
    const bool repeat = q.type == EarthOrbitType::repeating_ground_trace ||
                        q.type == EarthOrbitType::repeating_sun_sync;
    return {{q.epoch, a, e, i, angle(q.raan_rad), omega, angle(q.true_anomaly_rad)},
            j2,
            period,
            repeat ? q.repeat_revolutions * period : 0,
            r.node,
            r.apsis};
}
} // namespace orbelle::core
