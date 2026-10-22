#pragma once
#include <memory>
#include <orbelle/core/propagation/state_provider.hpp>
#include <orbelle/results/state_series.hpp>
#include <span>
#include <utility>

namespace orbelle::application {
// Immutable provider session. Scenario replaces whole sessions on an input revision.
class Session {
  public:
    Session(std::shared_ptr<const core::IStateProvider> provider, results::Provenance provenance);
    [[nodiscard]] results::StateSeries sample(std::span<const core::Epoch> epochs,
                                              const std::function<void()> &checkpoint = {}) const;
    [[nodiscard]] const results::Provenance &provenance() const noexcept { return provenance_; }
    // Shares the immutable provider and numerical cache; only result provenance changes.
    [[nodiscard]] Session with_input_revision(std::uint64_t revision) const {
        auto provenance = provenance_;
        provenance.input_revision = revision;
        return {provider_, std::move(provenance)};
    }
    [[nodiscard]] std::vector<core::Epoch>
    discontinuities(const std::function<void()> &checkpoint = {}) const {
        return provider_->discontinuities(checkpoint);
    }
    [[nodiscard]] std::vector<core::ManeuverEvent>
    maneuvers(core::Epoch first, core::Epoch last,
              const std::function<void()> &checkpoint = {}) const {
        return provider_->maneuvers(first, last, checkpoint);
    }

  private:
    std::shared_ptr<const core::IStateProvider> provider_;
    results::Provenance provenance_;
};
} // namespace orbelle::application
