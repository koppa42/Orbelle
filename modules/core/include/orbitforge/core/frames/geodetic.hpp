#pragma once
namespace orbitforge::core {
// WGS84 east-positive longitude [-pi, pi], geodetic latitude [-pi/2, pi/2]
// in radians and ellipsoid height in metres. Height is not altitude above terrain.
struct GeodeticPosition {
    double longitude_rad, latitude_rad, height_m;
};
} // namespace orbitforge::core
