#pragma once
#include "report_execution.hpp"
namespace orbitforge::runtime::detail {
void validate_coverage_assets(const application::Project &, const application::ReportQuery &);
struct CoverageReportData {
    std::vector<results::Row> rows;
    results::CoverageStatistics statistics;
};
CoverageReportData coverage_report(const ReportContext &, const application::ReportDefinition &,
                                   core::Epoch, core::Epoch, std::size_t, const ReportExecution &,
                                   const std::function<void(std::size_t, std::size_t)> &);
} // namespace orbitforge::runtime::detail
