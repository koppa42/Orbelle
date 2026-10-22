#pragma once
#include <array>
#include <functional>
#include <orbelle/core/propagation/access.hpp>
#include <string_view>
#include <vector>

namespace orbelle::core {
enum class LightingPhase { sunlight, penumbra, umbra };
[[nodiscard]] std::string_view lighting_phase_name(LightingPhase phase);
struct LightingGeometry {
    LightingPhase phase = LightingPhase::sunlight;
    double fraction = 1;
    // Signed discovery functions: negative inside partial-or-total / total shadow.
    // Their sign includes the same front/depth gates as the SRP geometry.
    double shadow_margin = 1, umbra_margin = 1;
};
// Earth-centred positions in the same inertial axes. Geometric Sun, spherical
// Earth (6378137 m), solar radius 695700000 m, uniform apparent solar disk.
// Reuses SRP disk geometry; independent of the satellite's force-model settings.
// Annular eclipses are penumbra. Observer must be outside both spheres.
[[nodiscard]] LightingGeometry evaluate_lighting(const std::array<double, 3> &satellite_m,
                                                 const std::array<double, 3> &sun_m);
struct LightingInterval {
    Epoch first{0}, last{0};
    LightingPhase phase = LightingPhase::sunlight;
    bool start_clipped = false, stop_clipped = false;
};
// A contiguous partition of [first,last], including full sunlight. Both shadow
// boundaries use scalar extremum-assisted discovery and bisection. Distinct lobes
// must be resolved by max_step; reduce it to check convergence. No zero-duration
// contacts. Returned boundary uncertainty <= tolerance; classified by interior.
// Exact TAI nanoseconds, finite callbacks, 200000 checks / 2000000 evaluations.
[[nodiscard]] std::vector<LightingInterval>
find_lighting_intervals(Epoch first, Epoch last, std::int64_t max_step_nanoseconds,
                        const std::function<LightingGeometry(Epoch)> &at,
                        std::int64_t tolerance_nanoseconds = 1'000'000,
                        AccessSearchMode mode = AccessSearchMode::extremum_assisted);
} // namespace orbelle::core
