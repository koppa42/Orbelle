#pragma once
#include <orbitforge/core/propagation/state_provider.hpp>

namespace orbitforge::core {
// WGS84 geodetic latitude, east-positive longitude (radians), ellipsoid height
// (metres). Terrestrial station heights supported by this model: -12 km to 100 km.
struct GroundStationLocation {
    double longitude_rad = 0;
    double latitude_rad = 0;
    double height_m = 0;
    bool operator==(const GroundStationLocation &) const = default;
};
[[nodiscard]] std::array<double, 3> ground_station_position(GroundStationLocation location);

// Fixed to the WGS84 ellipsoid in ITRF. No plate motion or tides. Transform the
// complete state, including velocity, when requesting a different reference frame.
class GroundStation final : public IStateProvider {
  public:
    explicit GroundStation(GroundStationLocation location);
    [[nodiscard]] CartesianState state_at(Epoch epoch) const override;

  private:
    std::array<double, 3> position_;
};
} // namespace orbitforge::core
