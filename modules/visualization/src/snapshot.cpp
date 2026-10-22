#include <cmath>
#include <orbelle/visualization/snapshot.hpp>
#include <stdexcept>
namespace orbelle::visualization {
RenderSnapshot make_snapshot(const results::StateSeries &series, std::size_t index) {
    const auto &state = series.states().at(index);
    return {state.epoch,
            state.frame,
            state.origin,
            {{series.provenance().object_id, state.position_m}}};
}
RenderSnapshot rotate_snapshot(const RenderSnapshot &snapshot,
                               const core::PositionRotation &rotation) {
    if (snapshot.epoch != rotation.epoch || snapshot.origin != rotation.origin ||
        snapshot.frame != rotation.from) {
        throw std::invalid_argument("Snapshot epoch, origin and frame must match the rotation");
    }
    auto result = snapshot;
    result.frame = rotation.to;
    for (auto &marker : result.markers) {
        for (const double value : marker.position_m) {
            if (!std::isfinite(value)) {
                throw std::invalid_argument("Display positions must be finite");
            }
        }
        marker.position_m = rotation.apply_position(marker.position_m);
        for (const double value : marker.position_m) {
            if (!std::isfinite(value)) {
                throw std::invalid_argument("Rotated display positions must be finite");
            }
        }
    }
    return result;
}
} // namespace orbelle::visualization
