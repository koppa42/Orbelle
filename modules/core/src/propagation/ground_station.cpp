#include <algorithm>
#include <cmath>
#include <numbers>
#include <orbelle/core/propagation/ground_station.hpp>
#include <stdexcept>

namespace orbelle::core {
namespace {
constexpr double a = 6378137.0, f = 1.0 / 298.257223563, e2 = f * (2 - f);
constexpr double b = a * (1 - f);

GroundStationLocation from_cartesian(const std::array<double, 3> &p) {
    const auto radius = std::hypot(p[0], p[1], p[2]);
    // Restrict the inverse to the supported near-surface shell, away from the
    // nonunique interior normal projection and overflow-prone input magnitudes.
    if (!std::isfinite(radius) || radius < b - 12000 - 1e-7 || radius > a + 100000 + 1e-7)
        throw std::invalid_argument("Ground coordinates are outside the supported WGS84 shell");
    const auto rho = std::hypot(p[0], p[1]);
    if (rho < 1e-9)
        return {0, std::copysign(std::numbers::pi / 2, p[2]), std::abs(p[2]) - b};
    const auto longitude = std::atan2(p[1], p[0]);
    auto latitude = std::atan2(p[2], rho * (1 - e2));
    // Fixed-point inversion of z = (N(1-e2)+h) sin(phi), convergent in this shell.
    for (int i = 0; i < 12; ++i) {
        const auto s = std::sin(latitude);
        const auto n = a / std::sqrt(1 - e2 * s * s);
        const auto next = std::atan2(p[2] + e2 * n * s, rho);
        if (std::abs(next - latitude) < 2e-16) {
            latitude = next;
            break;
        }
        latitude = next;
    }
    const auto s = std::sin(latitude), c = std::cos(latitude);
    // Projection on the normal avoids division by cos(phi) near the poles.
    auto height = rho * c + p[2] * s - a * std::sqrt(1 - e2 * s * s);
    // Roundoff at the two documented height boundaries is at the nanometre scale.
    if (height >= -12000 - 1e-7 && height <= 100000 + 1e-7)
        height = std::clamp(height, -12000.0, 100000.0);
    return {longitude, latitude, height};
}
} // namespace
std::array<double, 3> ground_station_position(GroundStationLocation location) {
    const auto [longitude, latitude, ground_height, above_ground] = location;
    const auto height = ground_height + above_ground;
    if (!std::isfinite(longitude) || !std::isfinite(latitude) || !std::isfinite(height) ||
        std::abs(longitude) > std::numbers::pi || std::abs(latitude) > std::numbers::pi / 2 ||
        !std::isfinite(ground_height) || !std::isfinite(above_ground) || ground_height < -12000 ||
        ground_height > 100000 || above_ground < 0 || above_ground > 100000 || height > 100000)
        throw std::invalid_argument("Ground station requires longitude [-180, 180] degrees, "
                                    "geodetic latitude [-90, 90] degrees, ground ellipsoid height "
                                    "[-12000, 100000] metres, height above ground [0, 100000] "
                                    "metres and total height <= 100000 metres");
    const auto sin_lat = std::sin(latitude), cos_lat = std::cos(latitude);
    const auto n = a / std::sqrt(1 - e2 * sin_lat * sin_lat);
    return {(n + height) * cos_lat * std::cos(longitude),
            (n + height) * cos_lat * std::sin(longitude), ((1 - e2) * n + height) * sin_lat};
}
std::array<double, 3> ground_station_coordinates(GroundStationLocation location,
                                                 GroundPositionType type) {
    (void)ground_station_position(location);
    location.height_above_ground_m = 0;
    const auto p = ground_station_position(location);
    switch (type) {
    case GroundPositionType::geodetic:
        return {location.latitude_rad, location.longitude_rad, location.height_m};
    case GroundPositionType::geocentric:
        return {
            std::atan2((1 - e2) * std::sin(location.latitude_rad), std::cos(location.latitude_rad)),
            location.longitude_rad, location.height_m};
    case GroundPositionType::cartesian:
        return p;
    case GroundPositionType::spherical:
        return {std::atan2(p[2], std::hypot(p[0], p[1])), location.longitude_rad,
                std::hypot(p[0], p[1], p[2])};
    case GroundPositionType::cylindrical:
        return {std::hypot(p[0], p[1]), location.longitude_rad, p[2]};
    }
    throw std::invalid_argument("Unsupported ground position type");
}
GroundStationLocation ground_station_location(GroundPositionType type,
                                              std::array<double, 3> coordinates,
                                              double above_ground) {
    const auto [x, y, z] = coordinates;
    if (!std::isfinite(x) || !std::isfinite(y) || !std::isfinite(z))
        throw std::invalid_argument("Ground coordinates must be finite");
    const auto angular = [&] {
        if (std::abs(x) > std::numbers::pi / 2 || std::abs(y) > std::numbers::pi)
            throw std::invalid_argument("Ground latitude/longitude are out of range");
    };
    GroundStationLocation result;
    switch (type) {
    case GroundPositionType::geodetic:
        result = {y, x, z};
        break;
    case GroundPositionType::geocentric:
        angular();
        result = {y, std::atan2(std::sin(x), (1 - e2) * std::cos(x)), z};
        break;
    case GroundPositionType::cartesian:
        result = from_cartesian(coordinates);
        break;
    case GroundPositionType::spherical:
        angular();
        if (z <= 0)
            throw std::invalid_argument("Spherical ground radius must be positive");
        result = from_cartesian(
            {z * std::cos(x) * std::cos(y), z * std::cos(x) * std::sin(y), z * std::sin(x)});
        result.longitude_rad = y; // Longitude remains defined at the poles.
        break;
    case GroundPositionType::cylindrical:
        if (x < 0 || std::abs(y) > std::numbers::pi)
            throw std::invalid_argument("Invalid cylindrical ground radius or longitude");
        result = from_cartesian({x * std::cos(y), x * std::sin(y), z});
        result.longitude_rad = y;
        break;
    default:
        throw std::invalid_argument("Unsupported ground position type");
    }
    result.height_above_ground_m = above_ground;
    (void)ground_station_position(result);
    return result;
}
GroundStation::GroundStation(GroundStationLocation location)
    : position_(ground_station_position(location)) {}
CartesianState GroundStation::state_at(Epoch epoch) const {
    return {epoch, OriginId::earth, FrameId::itrf, position_, {0, 0, 0}};
}
} // namespace orbelle::core
