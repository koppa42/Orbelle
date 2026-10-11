#pragma once
#include <array>
#include <orbitforge/core/frames/state.hpp>
#include <string>
#include <vector>

namespace orbitforge::application {
enum class ReportKind {
    scenario_summary,
    object_summary,
    cartesian_state,
    ground_stations,
    classical_elements,
    geodetic_position,
    orbital_motion
};
inline constexpr std::array report_kinds{
    ReportKind::scenario_summary, ReportKind::object_summary,     ReportKind::cartesian_state,
    ReportKind::ground_stations,  ReportKind::classical_elements, ReportKind::geodetic_position,
    ReportKind::orbital_motion};
[[nodiscard]] constexpr bool report_is_sampled(ReportKind kind) {
    return kind == ReportKind::cartesian_state || kind == ReportKind::classical_elements ||
           kind == ReportKind::geodetic_position || kind == ReportKind::orbital_motion;
}
[[nodiscard]] constexpr bool report_satellites_only(ReportKind kind) {
    return kind == ReportKind::classical_elements || kind == ReportKind::orbital_motion;
}
enum class ReportUnits { si, engineering };
enum class ReportTimeScale { utc, tai, tt };
// Stable built-in template IDs. Object IDs are bindings, never display names.
// An empty binding means all compatible objects, including objects inserted later.
struct ReportDefinition {
    std::string id, name;
    ReportKind kind = ReportKind::cartesian_state;
    std::vector<std::string> object_ids;
    bool follow_scenario = true;
    core::Epoch first{0}, last{0};
    std::int64_t step_nanoseconds = 60'000'000'000;
    // Used by Cartesian state only. Elements/motion fix GCRF; geodetic fixes ITRF/WGS84.
    core::FrameId frame = core::FrameId::gcrf;
    ReportUnits units = ReportUnits::engineering;
    ReportTimeScale time_scale = ReportTimeScale::utc;
};
void validate_report_definition(const ReportDefinition &definition);
} // namespace orbitforge::application
