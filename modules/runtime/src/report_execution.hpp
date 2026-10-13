#pragma once
#include <orbitforge/runtime/access.hpp>

namespace orbitforge::runtime::detail {
struct ReportObject {
    std::string id, name, kind;
    const application::ProjectGroundStation *station = nullptr;
    const application::ProjectSatellite *satellite = nullptr;
};
struct ReportAccess {
    std::string name;
    std::function<AccessSample(core::Epoch)> at;
};
// Query-specific sources; the table engine owns selection, sampling and formatting.
// Analysis supplies already prepared sessions; desktop reports create only selected sessions.
struct ReportExecution {
    std::function<application::Session(const ReportObject &, core::FrameId)> session;
    std::function<ReportAccess(const std::vector<std::string> &)> access;
    std::function<void()> check_cancelled;
};
[[nodiscard]] ReportResult
generate_report(const ReportContext &context, const application::ReportDefinition &definition,
                const ReportExecution &execution,
                const std::function<void(std::size_t, std::size_t)> &progress);
} // namespace orbitforge::runtime::detail
