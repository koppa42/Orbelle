#pragma once
#include <orbelle/visualization/snapshot.hpp>

namespace orbelle::visualization {
// Owned display-only Hermite samples. This is deliberately not an IStateProvider:
// analysis/event searches must query the original propagation model.
class StateTrack {
  public:
    explicit StateTrack(results::StateSeries samples);
    [[nodiscard]] RenderSnapshot sample(core::Epoch epoch) const;
    [[nodiscard]] core::CartesianState sample_state(core::Epoch epoch) const;
    [[nodiscard]] std::vector<RenderSnapshot> nodes() const;
    [[nodiscard]] const results::StateSeries &samples() const noexcept { return samples_; }

  private:
    results::StateSeries samples_;
};
} // namespace orbelle::visualization
