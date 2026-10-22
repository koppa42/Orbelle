#include <algorithm>
#include <cmath>
#include <orbelle/core/propagation/relative_motion.hpp>
#include <stdexcept>

namespace orbelle::core {
namespace {
using Vector = std::array<double, 3>;
double dot(const Vector &a, const Vector &b) { return a[0] * b[0] + a[1] * b[1] + a[2] * b[2]; }
double norm(const Vector &v) { return std::hypot(v[0], v[1], v[2]); }
Vector cross(const Vector &a, const Vector &b) {
    return {a[1] * b[2] - a[2] * b[1], a[2] * b[0] - a[0] * b[2], a[0] * b[1] - a[1] * b[0]};
}
void validate(const CartesianState &a, const CartesianState &b, FrameId frame) {
    if (a.epoch != b.epoch || a.origin != OriginId::earth || b.origin != OriginId::earth ||
        a.frame != frame || b.frame != frame)
        throw std::invalid_argument(
            "Relative analysis requires same-epoch Earth states in the declared frame");
    for (const auto *s : {&a, &b}) {
        for (const auto &v : {s->position_m, s->velocity_mps})
            for (double x : v)
                if (!std::isfinite(x))
                    throw std::invalid_argument("Relative analysis requires finite states");
        if (!std::isfinite(norm(s->position_m)) || norm(s->position_m) == 0)
            throw std::invalid_argument("Relative analysis requires nonzero finite positions");
    }
}
} // namespace
RelativeMotion relative_motion(const CartesianState &a, const CartesianState &b) {
    validate(a, b, FrameId::gcrf);
    const auto radius = norm(a.position_m), speed = norm(a.velocity_mps);
    if (!std::isfinite(speed) || speed == 0)
        throw std::invalid_argument("RIC axes require a nonzero primary velocity");
    Vector radial{}, velocity{}, delta{}, dv{};
    for (std::size_t i = 0; i < 3; ++i) {
        radial[i] = a.position_m[i] / radius;
        velocity[i] = a.velocity_mps[i] / speed;
        delta[i] = b.position_m[i] - a.position_m[i];
        dv[i] = b.velocity_mps[i] - a.velocity_mps[i];
    }
    auto normal = cross(radial, velocity);
    const auto h = norm(normal), range = norm(delta), relative_speed = norm(dv);
    if (h <= 1e-14 || !std::isfinite(h) || !std::isfinite(range) || !std::isfinite(relative_speed))
        throw std::invalid_argument(
            "RIC axes are undefined for radial trajectories or overflowing states");
    for (auto &x : normal)
        x /= h;
    const auto along = cross(normal, radial);
    std::optional<double> rate;
    if (range > 0) {
        Vector direction{};
        for (std::size_t i = 0; i < 3; ++i)
            direction[i] = delta[i] / range;
        rate = dot(direction, dv);
    }
    Vector ric{dot(delta, radial), dot(delta, along), dot(delta, normal)};
    if ((rate && !std::isfinite(*rate)) || !std::isfinite(norm(ric)))
        throw std::invalid_argument("Relative geometry overflow");
    return {range, relative_speed, rate, ric};
}
double intersatellite_clearance(const CartesianState &a, const CartesianState &b) {
    validate(a, b, FrameId::itrf);
    constexpr double equator = 6378137., pole = equator * (1 - 1 / 298.257223563);
    Vector p{}, delta{};
    for (std::size_t i = 0; i < 3; ++i) {
        const auto scale = i == 2 ? pole : equator;
        p[i] = a.position_m[i] / scale;
        delta[i] = b.position_m[i] / scale - p[i];
    }
    const auto dd = dot(delta, delta), pd = dot(p, delta);
    if (!std::isfinite(dd) || !std::isfinite(pd))
        throw std::invalid_argument("Intersatellite geometry overflow");
    const auto t = dd == 0 ? 0 : std::clamp(-pd / dd, 0., 1.);
    Vector nearest{};
    for (std::size_t i = 0; i < 3; ++i)
        nearest[i] = p[i] + t * delta[i];
    const auto margin = dot(nearest, nearest) - 1;
    if (!std::isfinite(margin))
        throw std::invalid_argument("Intersatellite geometry overflow");
    return margin;
}
} // namespace orbelle::core
