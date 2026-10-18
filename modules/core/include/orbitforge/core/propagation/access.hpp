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
// Enhanced discovery searches the constraint maximum and minimum in every check
// interval before refining crossings. Each check interval must resolve distinct
// extrema (unimodal lobes); reduce max_step and check convergence for fast motion.
// This is not a guarantee for arbitrary discontinuous or oscillatory functions.
enum class AccessSearchMode { fixed_grid, extremum_assisted };
struct AccessExtremum {
    Epoch epoch;
    double value;
};
struct PassExtrema {
    AccessExtremum maximum_elevation; // rad, station-local geodetic elevation
    AccessExtremum minimum_range;     // m, geometric slant range
};
[[nodiscard]] AccessExtremum find_access_extremum(Epoch first, Epoch last,
                                                  std::int64_t max_step_nanoseconds,
                                                  const std::function<double(Epoch)> &value,
                                                  bool maximum,
                                                  std::int64_t tolerance_nanoseconds = 1'000'000);
// Nonnegative margin means visible. Every evaluation must be finite. Evaluator
// exceptions propagate without publishing partial intervals. <= 2 million evaluations.
[[nodiscard]] std::vector<AccessInterval>
find_constraint_intervals(Epoch first, Epoch last, std::int64_t max_step_nanoseconds,
                          const std::function<double(Epoch)> &margin,
                          std::int64_t tolerance_nanoseconds = 1'000'000);
} // namespace orbitforge::core
