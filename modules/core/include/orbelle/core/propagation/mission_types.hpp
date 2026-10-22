#pragma once
#include <chrono>
#include <orbelle/core/frames/state.hpp>
#include <string>
#include <vector>

namespace orbelle::core {
enum class MissionSegmentKind { coast, impulse };
enum class MissionCoastModel { two_body, hpop };
// STK conventions: VNC = velocity, orbit normal, co-normal;
// LVLH = radial outward, transverse toward velocity, orbit normal (not VVLH).
enum class ManeuverFrame { gcrf, vnc, lvlh };
struct MissionSegment {
    MissionSegmentKind kind = MissionSegmentKind::coast;
    std::string name;
    std::int64_t duration_nanoseconds = 0;
    std::array<double, 3> delta_v_mps{};
    ManeuverFrame frame = ManeuverFrame::vnc;
    [[nodiscard]] static MissionSegment coast(std::chrono::nanoseconds duration,
                                              std::string name = {});
    [[nodiscard]] static MissionSegment impulse(std::array<double, 3> delta_v_mps,
                                                ManeuverFrame frame = ManeuverFrame::vnc,
                                                std::string name = {});
    bool operator==(const MissionSegment &) const = default;
};
struct MissionPlan {
    MissionCoastModel coast_model = MissionCoastModel::two_body;
    std::vector<MissionSegment> segments;
    bool operator==(const MissionPlan &) const = default;
};
struct ManeuverEvent {
    std::size_t segment_index; // Zero based in the saved sequence.
    std::string name;
    CartesianState before, after;
    std::array<double, 3> delta_v_gcrf_mps;
    // Sum of magnitudes from the initial epoch, including burns before a query interval.
    double cumulative_delta_v_mps;
};
// Validates a forward sequence of 1..256 segments, with positive coast duration,
// at most 366 days total, finite impulses, and no epoch overflow. Returns its end.
[[nodiscard]] Epoch validate_mission(Epoch initial, const MissionPlan &plan);
[[nodiscard]] CartesianState apply_impulse(const CartesianState &before,
                                           std::array<double, 3> delta_v_mps, ManeuverFrame frame);
} // namespace orbelle::core
