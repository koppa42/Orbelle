#pragma once
#include <memory>
#include <orbitforge/core/propagation/state_provider.hpp>
#include <vector>

namespace orbitforge::core {
enum class OrbitInterpolationMethod { lagrange, hermite };
struct OrbitInterpolationOptions {
    OrbitInterpolationMethod method = OrbitInterpolationMethod::hermite;
    // Polynomial degree: Lagrange 0..29, Hermite odd 3..29. These are not sample counts.
    int order = 7;
    bool operator==(const OrbitInterpolationOptions &) const = default;
};
using OrbitEphemerisArc = std::vector<CartesianState>;
void validate_orbit_interpolation(const OrbitInterpolationOptions &options);
// Owned, immutable scientific ephemeris; SI, TAI, one explicit origin/frame across all arcs.
// Velocities must be derivatives of positions in those axes with respect to TAI seconds.
// Arcs and their nodes must be strictly ordered. Arcs may touch but not overlap;
// at a shared endpoint the later arc wins (post-maneuver state). Gaps and extrapolation fail.
// Each arc needs order+1 Lagrange nodes or (order+1)/2 Hermite nodes; no order reduction.
// Exact nodes are returned unchanged. Otherwise retain both bracketing nodes, then expand
// to the nearest contiguous window (ties earlier). Order-zero Lagrange holds the left node.
// No window crosses an arc boundary.
// Invalid input throws invalid_argument; an uncovered epoch or numerically unresolved
// window throws PropagationError. Concurrent queries are independent and cancellable.
// This accepts explicit samples; it does not generate HPOP integration nodes or resample
// display tracks. The caller must split at force/data discontinuities as well as maneuvers.
[[nodiscard]] std::shared_ptr<const IStateProvider>
make_tabulated_orbit(std::vector<OrbitEphemerisArc> arcs, OrbitInterpolationOptions options = {},
                     const std::function<void()> &checkpoint = {});
} // namespace orbitforge::core
