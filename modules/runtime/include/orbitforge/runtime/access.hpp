#pragma once
#include <functional>
#include <orbitforge/core/propagation/access.hpp>
#include <orbitforge/results/aer_series.hpp>
#include <orbitforge/runtime/report.hpp>

namespace orbitforge::runtime {
using AccessSample = results::AerSample;
void validate_access_pair(const application::Project &project,
                          const std::vector<std::string> &object_ids);
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
} // namespace orbitforge::runtime
