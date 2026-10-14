#pragma once
#include <orbitforge/core/propagation/mission_types.hpp>
#include <orbitforge/results/state_series.hpp>
#include <utility>

namespace orbitforge::results {
// Owned burn records within the scenario interval. States and delta-v use GCRF/SI.
class ManeuverSeries {
  public:
    ManeuverSeries(std::vector<core::ManeuverEvent> events, Provenance provenance,
                   std::string input_sha256, std::string data_sha256)
        : events_(std::move(events)), provenance_(std::move(provenance)),
          input_(std::move(input_sha256)), data_(std::move(data_sha256)) {}
    [[nodiscard]] const auto &events() const { return events_; }
    [[nodiscard]] const auto &provenance() const { return provenance_; }
    [[nodiscard]] const auto &input_sha256() const { return input_; }
    [[nodiscard]] const auto &data_sha256() const { return data_; }

  private:
    std::vector<core::ManeuverEvent> events_;
    Provenance provenance_;
    std::string input_, data_;
};
} // namespace orbitforge::results
