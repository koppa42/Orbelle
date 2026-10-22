#pragma once
#include <orbelle/core/propagation/access.hpp>
#include <orbelle/results/access_windows.hpp>
#include <utility>

namespace orbelle::results {
// WGS84 station-local observation. Missing angles are undefined, never zero-filled.
struct AerSample {
    core::Epoch epoch;
    core::GroundObservation observation;
    std::optional<double> off_boresight_rad; // Only for a sensor target.
    bool visible;
    std::optional<double> sensor_margin_rad = {}; // Signed pattern clearance for Access search.
};
// Owns every sample and its provenance, including for an empty explicit-time query.
class AerSeries {
  public:
    AerSeries(std::vector<AerSample> samples, AccessProvenance provenance)
        : samples_(std::move(samples)), provenance_(std::move(provenance)) {}
    [[nodiscard]] const auto &samples() const { return samples_; }
    [[nodiscard]] const auto &provenance() const { return provenance_; }

  private:
    std::vector<AerSample> samples_;
    AccessProvenance provenance_;
};
} // namespace orbelle::results
