#pragma once
#include <orbitforge/core/propagation/hpop_variational.hpp>
#include <vector>

namespace orbitforge::core::detail {
// Internal streaming bridge: times are owned by the covariance sampler throughout
// callbacks. Results carry their original index, including duplicates.
void sample_hpop_variational(
    CartesianState initial, const std::vector<Epoch> &times, HpopOptions options,
    HpopEnvironment environment, HpopVariationalOptions variational,
    const std::function<void(std::size_t, const HpopVariationalState &)> &sample,
    const std::function<void()> &checkpoint);
} // namespace orbitforge::core::detail
