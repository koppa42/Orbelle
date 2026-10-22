#include <cmath>
#include <numbers>
#include <orbelle/core/propagation/ground_station.hpp>
#include <stdexcept>

namespace orbelle::core {
std::array<double, 3> ground_station_position(GroundStationLocation location) {
    const auto [longitude, latitude, height] = location;
    if (!std::isfinite(longitude) || !std::isfinite(latitude) || !std::isfinite(height) ||
        std::abs(longitude) > std::numbers::pi || std::abs(latitude) > std::numbers::pi / 2 ||
        height < -12000 || height > 100000)
        throw std::invalid_argument("Ground station requires longitude [-180, 180] degrees, "
                                    "geodetic latitude [-90, 90] degrees and ellipsoid height "
                                    "[-12000, 100000] metres");
    constexpr double a = 6378137.0, f = 1.0 / 298.257223563, e2 = f * (2 - f);
    const auto sin_lat = std::sin(latitude), cos_lat = std::cos(latitude);
    const auto n = a / std::sqrt(1 - e2 * sin_lat * sin_lat);
    return {(n + height) * cos_lat * std::cos(longitude),
            (n + height) * cos_lat * std::sin(longitude), ((1 - e2) * n + height) * sin_lat};
}
GroundStation::GroundStation(GroundStationLocation location)
    : position_(ground_station_position(location)) {}
CartesianState GroundStation::state_at(Epoch epoch) const {
    return {epoch, OriginId::earth, FrameId::itrf, position_, {0, 0, 0}};
}
} // namespace orbelle::core
