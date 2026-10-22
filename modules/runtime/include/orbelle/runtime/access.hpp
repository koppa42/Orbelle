#pragma once
#include <functional>
#include <orbelle/core/propagation/access.hpp>
#include <orbelle/results/aer_series.hpp>
#include <orbelle/runtime/report.hpp>

namespace orbelle::runtime {
using AccessSample = results::AerSample;
void validate_access_pair(const application::Project &project,
                          const std::vector<std::string> &object_ids);
// Shared by Analysis and reports. Evaluates the physical orbit at every candidate;
// never uses map/display samples. Callback exceptions cancel atomically.
[[nodiscard]] std::vector<results::AccessWindow> calculate_access_passes(
    const application::Project &project, const std::vector<std::string> &object_ids,
    const results::AccessQuery &query, const std::function<AccessSample(core::Epoch)> &at);
// Immutable calculation snapshot. Sensor is co-located with its satellite, with
// geocentric nadir or ideal instantaneous station tracking; no slew/attitude dynamics.
class AccessSession {
  public:
    AccessSession(ReportContext context, std::string station_id, std::string target_id);
    // The checkpoint also runs within numerical integration; throwing cancels the query.
    [[nodiscard]] AccessSample at(core::Epoch epoch,
                                  const std::function<void()> &checkpoint = {}) const;
    [[nodiscard]] std::string name() const;

  private:
    struct Impl;
    std::shared_ptr<const Impl> impl_;
};
} // namespace orbelle::runtime
