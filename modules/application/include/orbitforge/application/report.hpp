#pragma once
#include <array>
#include <orbitforge/core/frames/state.hpp>
#include <orbitforge/core/propagation/access.hpp>
#include <orbitforge/results/table.hpp>
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
    orbital_motion,
    access_intervals,
    aer,
    state_covariance,
    lighting_intervals,
    lighting_state,
    orbital_events
};
inline constexpr std::array report_kinds{
    ReportKind::scenario_summary, ReportKind::object_summary,     ReportKind::cartesian_state,
    ReportKind::ground_stations,  ReportKind::classical_elements, ReportKind::geodetic_position,
    ReportKind::orbital_motion,   ReportKind::access_intervals,   ReportKind::aer,
    ReportKind::state_covariance, ReportKind::lighting_intervals, ReportKind::lighting_state,
    ReportKind::orbital_events};
[[nodiscard]] constexpr bool report_is_lighting(ReportKind kind) {
    return kind == ReportKind::lighting_intervals || kind == ReportKind::lighting_state;
}
[[nodiscard]] constexpr bool report_is_interval(ReportKind kind) {
    return kind == ReportKind::access_intervals || kind == ReportKind::lighting_intervals;
}
[[nodiscard]] constexpr bool report_uses_event_search(ReportKind kind) {
    return report_is_interval(kind) || kind == ReportKind::orbital_events;
}
[[nodiscard]] constexpr bool report_is_access(ReportKind kind) {
    return kind == ReportKind::access_intervals || kind == ReportKind::aer;
}
[[nodiscard]] constexpr bool report_is_sampled(ReportKind kind) {
    return kind == ReportKind::cartesian_state || kind == ReportKind::classical_elements ||
           kind == ReportKind::geodetic_position || kind == ReportKind::orbital_motion ||
           report_is_access(kind) || kind == ReportKind::state_covariance ||
           report_is_lighting(kind) || kind == ReportKind::orbital_events;
}
[[nodiscard]] constexpr bool report_satellites_only(ReportKind kind) {
    return kind == ReportKind::classical_elements || kind == ReportKind::orbital_motion ||
           kind == ReportKind::state_covariance || report_is_lighting(kind) ||
           kind == ReportKind::orbital_events;
}
enum class ReportUnits { si, engineering };
enum class ReportTimeScale { utc, tai, tt };
enum class ReportScope { objects, all_compatible };
// Object IDs are bindings, never display names. All-object selection is explicit.
// Access/AER require exactly [ground station ID, satellite or sensor ID].
struct ReportQuery {
    ReportKind kind = ReportKind::cartesian_state;
    ReportScope scope = ReportScope::objects;
    std::vector<std::string> object_ids;
    bool follow_scenario = true;
    core::Epoch first{0}, last{0};
    std::int64_t step_nanoseconds = 60'000'000'000;
    // Used by Cartesian state only. Elements/motion fix GCRF; geodetic fixes ITRF/WGS84.
    core::FrameId frame = core::FrameId::gcrf;
    core::AccessSearchMode event_search_mode = core::AccessSearchMode::extremum_assisted;
    std::int64_t event_tolerance_nanoseconds = 1'000'000;
    // Lighting uses an explicitly prepared, immutable solar ephemeris.
    std::string lighting_ephemeris = "de440s";
    bool operator==(const ReportQuery &) const = default;
};
struct ReportColumnSpec {
    std::string field, unit, title;
    bool operator==(const ReportColumnSpec &) const = default;
};
struct ReportTemplate {
    // Empty means the complete schema. Empty unit/title uses the field default.
    std::vector<ReportColumnSpec> columns;
    ReportUnits units = ReportUnits::engineering;
    ReportTimeScale time_scale = ReportTimeScale::utc;
    bool operator==(const ReportTemplate &) const = default;
};
struct ReportDefinition {
    std::string id, name;
    ReportQuery query;
    ReportTemplate format;
};
[[nodiscard]] std::vector<results::Column> report_columns(ReportKind kind);
[[nodiscard]] bool report_uses_utc(const ReportTemplate &format, ReportKind kind);
void validate_report_query(const ReportQuery &query);
void validate_report_template(const ReportTemplate &format, ReportKind kind);
void validate_report_definition(const ReportDefinition &definition);
} // namespace orbitforge::application
