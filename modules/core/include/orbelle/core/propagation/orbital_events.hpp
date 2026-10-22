#pragma once
#include <functional>
#include <orbelle/core/propagation/access.hpp>
#include <orbelle/core/units/quantities.hpp>
#include <string_view>
#include <vector>

namespace orbelle::core {
enum class OrbitalEventKind { periapsis, apoapsis, ascending_node, descending_node };
[[nodiscard]] std::string_view orbital_event_name(OrbitalEventKind kind);
struct OrbitalEvent {
    OrbitalEventKind kind;
    CartesianState state; // Exact query state at the refined event epoch, Earth/GCRF, SI.
};
struct OrbitalEvents {
    std::vector<OrbitalEvent> events;
    // At least one evaluated state is below the corresponding definition threshold.
    bool near_circular = false, near_equatorial = false;
};
// Peri/apo: radial velocity increases/decreases through zero. Nodes: GCRF z
// increases/decreases through zero. No sampled extrema or osculating event times.
// At candidate events, e <= 1e-8 suppresses apsides; |sin(i)| <= 1e-8 suppresses
// nodes. These explicit definition thresholds are not STK's undocumented values.
// Continuous Earth/GCRF states required; callbacks may throw to cancel. Includes
// isolated exact endpoint zeros; a single-epoch query has no crossing events.
// Max step must separate distinct lobes; compare halved steps for convergence.
// <= 200000 checks and 2000000 evaluations; invalid/overflowing inputs rejected.
[[nodiscard]] OrbitalEvents
find_orbital_events(Epoch first, Epoch last, std::int64_t max_step_nanoseconds,
                    const std::function<CartesianState(Epoch)> &at, GravitationalParameter mu,
                    std::int64_t tolerance_nanoseconds = 1'000'000,
                    AccessSearchMode mode = AccessSearchMode::extremum_assisted);
} // namespace orbelle::core
