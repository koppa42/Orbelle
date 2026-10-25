#pragma once
#include "report_execution.hpp"
namespace orbelle::runtime::detail {
void validate_coverage_assets(const application::Project &, const application::ReportQuery &);
using CoverageReportPlan = CoverageReportEstimate;
inline constexpr std::size_t coverage_workspace_bytes = 64ULL * 1024 * 1024;
inline constexpr std::size_t coverage_window_storage_bytes = 6 * sizeof(core::AccessInterval);
struct CoverageReportData {
    std::vector<results::Row> rows;
    results::CoverageStatistics statistics;
};
CoverageReportData coverage_report(const ReportContext &, const application::ReportDefinition &,
                                   core::Epoch, core::Epoch, const CoverageReportPlan &,
                                   const ReportExecution &,
                                   const std::function<void(std::size_t, std::size_t)> &);
} // namespace orbelle::runtime::detail
