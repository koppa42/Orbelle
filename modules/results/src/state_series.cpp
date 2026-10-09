#include <orbitforge/results/state_series.hpp>
#include <utility>
namespace orbitforge::results {
StateSeries::StateSeries(Provenance provenance, std::vector<core::CartesianState> states)
    : provenance_(std::move(provenance)), states_(std::move(states)) {}
const Provenance &StateSeries::provenance() const noexcept { return provenance_; }
const std::vector<core::CartesianState> &StateSeries::states() const noexcept { return states_; }
} // namespace orbitforge::results
