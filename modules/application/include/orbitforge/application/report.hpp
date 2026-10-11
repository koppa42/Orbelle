#pragma once
#include <orbitforge/core/frames/state.hpp>
#include <string>
#include <vector>

namespace orbitforge::application {
enum class ReportKind { scenario_summary, object_summary, cartesian_state, ground_stations };
enum class ReportUnits { si, engineering };
enum class ReportTimeScale { utc, tai, tt };
// Version 1 built-in templates. Object IDs are bindings, never display names.
// An empty binding means all compatible objects, including objects inserted later.
struct ReportDefinition {
    std::string id, name;
    ReportKind kind = ReportKind::cartesian_state;
    std::vector<std::string> object_ids;
    bool follow_scenario = true;
    core::Epoch first{0}, last{0};
    std::int64_t step_nanoseconds = 60'000'000'000;
    core::FrameId frame = core::FrameId::gcrf;
    ReportUnits units = ReportUnits::engineering;
    ReportTimeScale time_scale = ReportTimeScale::utc;
};
void validate_report_definition(const ReportDefinition &definition);
} // namespace orbitforge::application
