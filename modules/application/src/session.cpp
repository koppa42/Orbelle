#include <orbitforge/application/session.hpp>
#include <stdexcept>
#include <utility>
namespace orbitforge::application {
Session::Session(std::shared_ptr<const core::IStateProvider> provider,
                 results::Provenance provenance)
    : provider_(std::move(provider)), provenance_(std::move(provenance)) {
    if (!provider_) {
        throw std::invalid_argument("A session requires a state provider");
    }
    if (provenance_.object_id.empty() || provenance_.algorithm.empty()) {
        throw std::invalid_argument("Result provenance must identify the object and algorithm");
    }
}
results::StateSeries Session::sample(std::span<const core::Epoch> epochs,
                                     const std::function<void()> &checkpoint) const {
    std::vector<core::CartesianState> states;
    states.reserve(epochs.size());
    for (const auto epoch : epochs) {
        states.push_back(provider_->state_at(epoch, checkpoint));
    }
    return {provenance_, std::move(states)};
}
} // namespace orbitforge::application
