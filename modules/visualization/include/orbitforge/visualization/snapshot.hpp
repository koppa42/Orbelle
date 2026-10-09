#pragma once
#include <cstddef>
#include <orbitforge/core/frames/frame_provider.hpp>
#include <orbitforge/results/state_series.hpp>
#include <string>
#include <vector>

namespace orbitforge::visualization {
struct Marker {
    std::string object_id;
    std::array<double, 3> position_m;
};
struct RenderSnapshot {
    core::Epoch epoch;
    core::FrameId frame;
    core::OriginId origin;
    std::vector<Marker> markers;
};
// P0 adapter only: one object's sampled state, no interpolation or LOD yet.
// Publish as const; rendering implementations must not modify a published snapshot.
[[nodiscard]] RenderSnapshot make_snapshot(const results::StateSeries &series, std::size_t index);
// Explicit position-only frame conversion. The source snapshot remains unchanged.
[[nodiscard]] RenderSnapshot rotate_snapshot(const RenderSnapshot &snapshot,
                                             const core::PositionRotation &rotation);
} // namespace orbitforge::visualization
