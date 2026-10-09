#pragma once
#include <memory>
#include <orbitforge/core/propagation/state_provider.hpp>
#include <orbitforge/results/state_series.hpp>
#include <span>

namespace orbitforge::application {
// Minimal read-only P0 session. Editing, jobs and result publication are not implemented yet.
class Session {
  public:
    Session(std::shared_ptr<const core::IStateProvider> provider, results::Provenance provenance);
    [[nodiscard]] results::StateSeries sample(std::span<const core::Epoch> epochs) const;

  private:
    std::shared_ptr<const core::IStateProvider> provider_;
    results::Provenance provenance_;
};
} // namespace orbitforge::application
