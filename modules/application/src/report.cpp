#include <orbitforge/application/report.hpp>
#include <set>
#include <stdexcept>

namespace orbitforge::application {
void validate_report_definition(const ReportDefinition &d) {
    if (d.id.empty() || d.name.empty() || d.id.size() > 16384 || d.name.size() > 16384 ||
        d.kind < ReportKind::scenario_summary || d.kind > ReportKind::orbital_motion ||
        d.units < ReportUnits::si || d.units > ReportUnits::engineering ||
        d.time_scale < ReportTimeScale::utc || d.time_scale > ReportTimeScale::tt ||
        (d.frame != core::FrameId::gcrf && d.frame != core::FrameId::itrf) ||
        d.step_nanoseconds < 1 || d.step_nanoseconds > 86400LL * 1'000'000'000 ||
        (!d.follow_scenario && d.first.tai_nanoseconds() > d.last.tai_nanoseconds()) ||
        d.object_ids.size() > 10000)
        throw std::invalid_argument("Invalid report definition");
    std::set<std::string> ids;
    for (const auto &id : d.object_ids)
        if (id.empty() || id.size() > 16384 || !ids.insert(id).second)
            throw std::invalid_argument("Invalid or duplicate report object ID");
}
} // namespace orbitforge::application
