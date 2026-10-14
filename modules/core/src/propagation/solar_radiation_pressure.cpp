#include <algorithm>
#include <cmath>
#include <limits>
#include <numbers>
#include <orbitforge/core/propagation/solar_radiation_pressure.hpp>
#include <orbitforge/core/propagation/state_provider.hpp>
#include <span>
#include <utility>

namespace orbitforge::core {
namespace {
using Vector = std::array<double, 3>;
constexpr double pi = std::numbers::pi, turn = 2 * pi;
Vector subtract(const Vector &a, const Vector &b) {
    return {a[0] - b[0], a[1] - b[1], a[2] - b[2]};
}
double dot(const Vector &a, const Vector &b) { return a[0] * b[0] + a[1] * b[1] + a[2] * b[2]; }
Vector cross(const Vector &a, const Vector &b) {
    return {a[1] * b[2] - a[2] * b[1], a[2] * b[0] - a[0] * b[2], a[0] * b[1] - a[1] * b[0]};
}
double norm(const Vector &v) { return std::hypot(v[0], v[1], v[2]); }
Vector unit(const Vector &v, double distance) {
    return {v[0] / distance, v[1] / distance, v[2] / distance};
}
void finite_vector(const Vector &v) {
    if (!std::all_of(v.begin(), v.end(), [](double x) { return std::isfinite(x); }))
        throw PropagationError("SRP requires finite geometric positions");
}
struct Disk {
    double x, y, radius;
};
bool arc_inside(double x, double y, const Disk &boundary, const Disk &disk) {
    const double distance = std::hypot(boundary.x - disk.x, boundary.y - disk.y);
    // Membership of a tangent/nonintersecting boundary is constant except at
    // isolated points. A midpoint landing on such a point must not classify a full arc.
    if (distance >= boundary.radius + disk.radius)
        return false;
    if (distance <= std::abs(boundary.radius - disk.radius))
        return boundary.radius <= disk.radius;
    return std::hypot(x - disk.x, y - disk.y) < disk.radius;
}
double angle(double value) {
    value = std::fmod(value, turn);
    return value < 0 ? value + turn : value;
}
// Green's theorem integrates the exposed boundary arcs of the clipped disk union.
// Circle intersections partition each boundary into arcs with constant membership.
double covered_area(std::span<const Disk> occultors) {
    std::array<Disk, 3> storage{{{0, 0, 1}}};
    std::size_t count = 1;
    for (std::size_t i = 0; i < occultors.size(); ++i) {
        const auto &disk = occultors[i];
        if (std::hypot(disk.x, disk.y) >= 1 + disk.radius)
            continue;
        if (std::hypot(disk.x, disk.y) + 1 <= disk.radius)
            return pi;
        bool contained = false;
        for (std::size_t j = 0; j < occultors.size(); ++j) {
            if (i == j)
                continue;
            const auto &other = occultors[j];
            const double tolerance = 64 * std::numeric_limits<double>::epsilon() *
                                     std::max({1.0, disk.radius, other.radius});
            if (j < i && std::hypot(disk.x - other.x, disk.y - other.y) <= tolerance &&
                std::abs(disk.radius - other.radius) <= tolerance) {
                contained = true;
                break;
            }
            if (std::hypot(disk.x - other.x, disk.y - other.y) + disk.radius <= other.radius &&
                (disk.radius < other.radius || j < i)) {
                contained = true;
                break;
            }
        }
        if (!contained)
            storage[count++] = disk;
    }
    const auto circles = std::span{storage}.first(count);
    double area = 0;
    for (std::size_t i = 0; i < circles.size(); ++i) {
        const auto &circle = circles[i];
        std::array<double, 6> edge_storage{0, turn};
        std::size_t edge_count = 2;
        for (std::size_t j = 0; j < circles.size(); ++j) {
            if (i == j)
                continue;
            const auto &other = circles[j];
            const double dx = other.x - circle.x, dy = other.y - circle.y;
            const double d = std::hypot(dx, dy);
            if (d <= std::abs(circle.radius - other.radius) || d >= circle.radius + other.radius)
                continue;
            const double direction = std::atan2(dy, dx);
            const double opening = std::acos(
                std::clamp((circle.radius * circle.radius + d * d - other.radius * other.radius) /
                               (2 * circle.radius * d),
                           -1.0, 1.0));
            edge_storage[edge_count++] = angle(direction - opening);
            edge_storage[edge_count++] = angle(direction + opening);
        }
        auto edges = std::span{edge_storage}.first(edge_count);
        // At most six endpoints: direct insertion keeps the small stack buffer bounded
        // and avoids generic std::sort's larger insertion-sort threshold on GCC 13.
        for (std::size_t k = 1; k < edges.size(); ++k) {
            const double value = edges[k];
            auto position = k;
            while (position > 0 && edges[position - 1] > value) {
                edges[position] = edges[position - 1];
                --position;
            }
            edges[position] = value;
        }
        for (std::size_t k = 1; k < edges.size(); ++k) {
            const double start = edges[k - 1], end = edges[k], middle = (start + end) / 2;
            const double x = circle.x + circle.radius * std::cos(middle);
            const double y = circle.y + circle.radius * std::sin(middle);
            bool boundary = i == 0 ? false : arc_inside(x, y, circle, circles[0]);
            for (std::size_t j = 1; j < circles.size(); ++j) {
                if (i == j)
                    continue;
                if (arc_inside(x, y, circle, circles[j])) {
                    boundary = i == 0;
                    if (i != 0)
                        break;
                }
            }
            if (boundary)
                area += 0.5 * (circle.radius * circle.radius * (end - start) +
                               circle.radius * circle.x * (std::sin(end) - std::sin(start)) +
                               circle.radius * circle.y * (std::cos(start) - std::cos(end)));
        }
    }
    return std::clamp(area, 0.0, pi);
}
} // namespace
void validate_srp_options(const SrpOptions &o) {
    if (o.shadow_model != SrpShadowModel::none && o.shadow_model != SrpShadowModel::cylindrical &&
        o.shadow_model != SrpShadowModel::dual_cone)
        throw std::invalid_argument("Unsupported SRP shadow model");
    if (!std::isfinite(o.cr) || o.cr < -100 || o.cr > 100)
        throw std::invalid_argument("SRP Cr must be finite and within -100..100");
    if (!std::isfinite(o.area_mass_ratio_m2_per_kg) || o.area_mass_ratio_m2_per_kg < 0 ||
        o.area_mass_ratio_m2_per_kg > 9999)
        throw std::invalid_argument("SRP area/mass ratio must be within 0..9999 m^2/kg");
    for (double value : {o.pressure_at_reference_pa, o.reference_distance_m, o.sun_radius_m,
                         o.earth_radius_m, o.moon_radius_m})
        if (!std::isfinite(value) || value <= 0)
            throw std::invalid_argument(
                "SRP pressure, reference distance and radii must be finite and positive");
    if (!std::isfinite(o.atmospheric_altitude_m) || o.atmospheric_altitude_m < 0 ||
        !std::isfinite(o.earth_radius_m + o.atmospheric_altitude_m))
        throw std::invalid_argument("SRP atmospheric altitude must be finite and nonnegative");
}
SrpOptions effective_srp_options(SrpOptions options) {
    if (!options.enabled)
        return {};
    const SrpOptions defaults;
    if (options.shadow_model == SrpShadowModel::none)
        options.earth_shadow = options.moon_shadow = false;
    if (!options.earth_shadow) {
        options.earth_radius_m = defaults.earth_radius_m;
        options.atmospheric_altitude_m = 0;
    }
    if (!options.moon_shadow)
        options.moon_radius_m = defaults.moon_radius_m;
    return options;
}
SrpEvaluation evaluate_srp(const Vector &satellite, const Vector &sun, const SrpOptions &options,
                           const std::optional<Vector> &moon) {
    validate_srp_options(options);
    if (!options.enabled)
        return {};
    finite_vector(satellite);
    finite_vector(sun);
    const auto to_sun = subtract(sun, satellite);
    const double sun_distance = norm(to_sun);
    if (!std::isfinite(sun_distance) || sun_distance <= options.sun_radius_m)
        throw PropagationError("SRP observer must be outside the solar sphere");
    const auto direction = unit(to_sun, sun_distance);
    double lighting = 1;
    std::array<std::pair<Vector, double>, 2> bodies;
    std::size_t body_count = 0;
    if (options.shadow_model != SrpShadowModel::none) {
        if (options.earth_shadow)
            bodies[body_count++] = {subtract(Vector{}, satellite),
                                    options.earth_radius_m + options.atmospheric_altitude_m};
        if (options.moon_shadow) {
            if (!moon)
                throw std::invalid_argument("SRP Moon shadow requires geometric Moon positions");
            finite_vector(*moon);
            bodies[body_count++] = {subtract(*moon, satellite), options.moon_radius_m};
        }
    }
    std::array<Disk, 2> disks;
    std::size_t disk_count = 0;
    const double sun_angle = std::asin(options.sun_radius_m / sun_distance);
    const Vector axis = std::abs(direction[2]) < 0.8 ? Vector{0, 0, 1} : Vector{0, 1, 0};
    auto x_axis = cross(direction, axis);
    x_axis = unit(x_axis, norm(x_axis));
    const auto y_axis = cross(direction, x_axis);
    for (const auto &[to_body, radius] : std::span{bodies}.first(body_count)) {
        const double distance = norm(to_body);
        if (!std::isfinite(distance) || distance <= radius)
            throw PropagationError("SRP observer must be outside the assigned occulting sphere");
        const double along = dot(to_body, direction);
        if (along <= 0 || distance >= sun_distance)
            continue;
        if (options.shadow_model == SrpShadowModel::cylindrical) {
            if (norm(cross(to_body, direction)) <= radius) {
                lighting = 0;
                break;
            }
        } else {
            const auto body_direction = unit(to_body, distance);
            const double separation =
                std::atan2(norm(cross(direction, body_direction)), dot(direction, body_direction));
            const double apparent_radius = std::asin(radius / distance);
            if (separation >= sun_angle + apparent_radius)
                continue;
            const double tx = dot(body_direction, x_axis), ty = dot(body_direction, y_axis);
            const double transverse = std::hypot(tx, ty);
            const double scale = transverse > 0 ? separation / (transverse * sun_angle) : 0;
            disks[disk_count++] = {tx * scale, ty * scale, apparent_radius / sun_angle};
        }
    }
    if (options.shadow_model == SrpShadowModel::dual_cone)
        lighting = 1 - covered_area(std::span{disks}.first(disk_count)) / pi;
    const double ratio = options.reference_distance_m / sun_distance;
    const double magnitude = lighting * options.pressure_at_reference_pa * ratio * ratio *
                             options.cr * options.area_mass_ratio_m2_per_kg;
    Vector acceleration{-magnitude * direction[0], -magnitude * direction[1],
                        -magnitude * direction[2]};
    finite_vector(acceleration);
    return {acceleration, lighting};
}
} // namespace orbitforge::core
