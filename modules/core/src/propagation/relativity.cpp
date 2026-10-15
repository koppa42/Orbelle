#include <cmath>
#include <orbitforge/core/propagation/relativity.hpp>
#include <stdexcept>

namespace orbitforge::core {
namespace {
using Vector = std::array<double, 3>;
constexpr double c = 299792458, angular_momentum_per_mass = 9.8e8;
double dot(const Vector &a, const Vector &b) { return a[0] * b[0] + a[1] * b[1] + a[2] * b[2]; }
Vector cross(const Vector &a, const Vector &b) {
    return {a[1] * b[2] - a[2] * b[1], a[2] * b[0] - a[0] * b[2], a[0] * b[1] - a[1] * b[0]};
}
double norm(const Vector &v) { return std::hypot(v[0], v[1], v[2]); }
void finite(const Vector &v) {
    for (double x : v)
        if (!std::isfinite(x))
            throw std::invalid_argument("Relativistic force requires finite vectors");
}
} // namespace
RelativityEvaluation evaluate_relativity(const Vector &r, const Vector &v, double earth_mu,
                                         const Vector &sun, const Vector &sun_velocity,
                                         double sun_mu, const Vector &pole) {
    for (const auto *input : {&r, &v, &sun, &sun_velocity, &pole})
        finite(*input);
    const double radius = norm(r), distance = norm(sun), speed = norm(v),
                 sun_speed = norm(sun_velocity);
    if (!std::isfinite(earth_mu) || earth_mu <= 0 || !std::isfinite(sun_mu) || sun_mu <= 0 ||
        !std::isfinite(radius) || radius <= 0 || !std::isfinite(distance) || distance <= 0 ||
        speed >= c || sun_speed >= c || std::abs(norm(pole) - 1) > 1e-12)
        throw std::invalid_argument("Invalid relativistic force GM, radius, velocity or unit pole");
    const double scale = earth_mu / (c * c * radius * radius * radius);
    const double radial = 4 * earth_mu / radius - dot(v, v), along = 4 * dot(r, v);
    const auto orbit_normal = cross(r, v), v_cross_pole = cross(v, pole);
    const double projection = 3 * dot(r, pole) / (radius * radius);
    // Reversing both Earth-relative Sun vectors leaves this cross product unchanged.
    const auto solar_precession = cross(cross(sun, sun_velocity), v);
    const double solar_scale = 3 * sun_mu / (c * c * distance * distance * distance);
    RelativityEvaluation result{};
    for (std::size_t i = 0; i < 3; ++i) {
        result.schwarzschild_mps2[i] = scale * (radial * r[i] + along * v[i]);
        result.lense_thirring_mps2[i] = 2 * scale * angular_momentum_per_mass *
                                        (projection * orbit_normal[i] + v_cross_pole[i]);
        result.de_sitter_mps2[i] = solar_scale * solar_precession[i];
        result.acceleration_mps2[i] =
            result.schwarzschild_mps2[i] + result.lense_thirring_mps2[i] + result.de_sitter_mps2[i];
    }
    finite(result.schwarzschild_mps2);
    finite(result.lense_thirring_mps2);
    finite(result.de_sitter_mps2);
    finite(result.acceleration_mps2);
    return result;
}
} // namespace orbitforge::core
