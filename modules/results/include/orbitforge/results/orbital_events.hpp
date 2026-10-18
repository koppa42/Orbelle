#pragma once
#include <array>
#include <cstddef>
namespace orbitforge::results {
struct OrbitalEventStatistics {
    std::size_t object_count = 0;
    // Periapsis, apoapsis, ascending node, descending node; counts across objects.
    std::array<std::size_t, 4> counts{};
    // Objects with at least one evaluated near-circular / near-equatorial state.
    std::size_t near_circular_objects = 0, near_equatorial_objects = 0;
};
} // namespace orbitforge::results
