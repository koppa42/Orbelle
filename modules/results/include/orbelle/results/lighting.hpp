#pragma once
#include <array>
#include <cstddef>
namespace orbelle::results {
struct LightingPhaseStatistics {
    std::size_t count = 0;
    double total_duration_s = 0, longest_duration_s = 0, fraction = 0;
};
// Sunlight, penumbra, umbra in enum order. For multiple satellites durations sum
// independently; fraction uses total analysed object-time. Includes clipped parts.
struct LightingStatistics {
    std::size_t object_count = 0;
    std::array<LightingPhaseStatistics, 3> phases{};
};
} // namespace orbelle::results
