#pragma once
#include "report_execution.hpp"
namespace orbitforge::runtime::detail {
struct IntersatelliteReportData {
    std::vector<results::Row> rows;
    results::IntersatelliteStatistics statistics;
};
IntersatelliteReportData
intersatellite_report(const ReportContext &, const application::ReportDefinition &, core::Epoch,
                      core::Epoch, std::size_t, const ReportExecution &,
                      const std::function<void(std::size_t, std::size_t)> &);
} // namespace orbitforge::runtime::detail
