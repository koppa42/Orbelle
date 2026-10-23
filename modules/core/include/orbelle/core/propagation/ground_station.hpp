#pragma once
#include <orbelle/core/propagation/state_provider.hpp>

namespace orbelle::core {
// WGS84 ground point: east-positive longitude and geodetic latitude (radians),
// ground ellipsoid height [-12 km, 100 km], and height above that ground [0, 100 km].
// The observer is displaced along the ellipsoid normal; its total height must be
// <= 100 km. No terrain or mean-sea-level conversion is implied.
struct GroundStationLocation {
    double longitude_rad = 0;
    double latitude_rad = 0;
    double height_m = 0;
    double height_above_ground_m = 0;
    bool operator==(const GroundStationLocation &) const = default;
};
[[nodiscard]] std::array<double, 3> ground_station_position(GroundStationLocation location);

enum class GroundPositionType { geodetic, geocentric, cartesian, spherical, cylindrical };
// Ground-point coordinates in SI units, excluding height_above_ground_m:
// geodetic/geocentric: latitude, longitude, ellipsoid height;
// cartesian: ITRF X, Y, Z; spherical: vector latitude, longitude, geocentric radius;
// cylindrical: distance from polar axis, longitude, Z.
// Geocentric latitude is that of the normal-projected ellipsoid subpoint (STK
// convention), not the spherical latitude of the elevated position vector.
// Invalid/nonfinite coordinates or unsupported terrestrial heights throw invalid_argument.
[[nodiscard]] std::array<double, 3> ground_station_coordinates(GroundStationLocation location,
                                                               GroundPositionType type);
[[nodiscard]] GroundStationLocation ground_station_location(GroundPositionType type,
                                                            std::array<double, 3> coordinates,
                                                            double height_above_ground_m = 0);

// Fixed to the WGS84 ellipsoid in ITRF. No plate motion or tides. Transform the
// complete state, including velocity, when requesting a different reference frame.
class GroundStation final : public IStateProvider {
  public:
    explicit GroundStation(GroundStationLocation location);
    [[nodiscard]] CartesianState state_at(Epoch epoch) const override;

  private:
    std::array<double, 3> position_;
};
} // namespace orbelle::core
