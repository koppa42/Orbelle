#pragma once
#include <functional>
#include <optional>
#include <orbitforge/core/propagation/ground_station.hpp>
#include <vector>

namespace orbitforge::core {
struct GroundObservation {
    // Local geodetic ENU: north through east in [0, 2pi); undefined at zenith/nadir.
    std::optional<double> azimuth_rad;
    double elevation_rad, range_m, range_rate_mps;
    bool line_of_sight; // WGS84 ellipsoid, no terrain, refraction or light time.
};
// Target must be Earth-centred ITRF, in SI. Station is fixed in ITRF.
[[nodiscard]] GroundObservation observe_ground_target(GroundStationLocation station,
                                                      const CartesianState &target);
// Angle between two nonzero vectors, in radians. Rejects nonfinite inputs.
[[nodiscard]] double vector_angle(const std::array<double, 3> &a, const std::array<double, 3> &b);
struct AccessInterval {
    Epoch first, last;
    bool start_clipped = false, stop_clipped = false;
};
// Fixed discovery grid including exact endpoints; brackets transitions and bisects to
// tolerance. Windows/gaps shorter than step may be missed. Returned edges are on the
// visible side of each bracket. Isolated zero-duration contacts are omitted.
// Evaluator may throw to cancel; no partial result is returned. At most 200000 grid points.
[[nodiscard]] std::vector<AccessInterval>
find_access_intervals(Epoch first, Epoch last, std::int64_t step_nanoseconds,
                      const std::function<bool(Epoch)> &visible,
                      std::int64_t tolerance_nanoseconds = 1'000'000);
} // namespace orbitforge::core
