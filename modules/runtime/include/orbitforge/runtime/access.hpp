#pragma once
#include <orbitforge/core/propagation/access.hpp>
#include <orbitforge/runtime/report.hpp>

namespace orbitforge::runtime {
struct AccessSample {
    core::Epoch epoch;
    core::GroundObservation observation;
    std::optional<double> off_boresight_rad; // Only for a sensor target.
    bool visible;
};
void validate_access_pair(const application::Project &project,
                          const std::vector<std::string> &object_ids);
// Immutable calculation snapshot. Sensor is co-located with its satellite, with
// geocentric nadir or ideal instantaneous station tracking; no slew/attitude dynamics.
class AccessSession {
  public:
    AccessSession(ReportContext context, std::string station_id, std::string target_id);
    [[nodiscard]] AccessSample at(core::Epoch epoch) const;
    [[nodiscard]] std::string name() const;

  private:
    struct Impl;
    std::shared_ptr<const Impl> impl_;
};
} // namespace orbitforge::runtime
