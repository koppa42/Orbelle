#pragma once
#include <memory>
#include <orbitforge/core/propagation/state_provider.hpp>

namespace orbitforge::core {
using CoastFactory = std::function<std::shared_ptr<const IStateProvider>(CartesianState)>;
// Each impulse restarts the supplied coast model with its post-burn state.
// Queries are restricted to [initial, plan end], with the post-burn state at an
// exact burn epoch. Consecutive impulses at one epoch execute in sequence order.
// Immutable configuration; completed arcs are cached under a mutex. Cancellation
// throws without returning partial results and a later query can safely retry.
[[nodiscard]] std::shared_ptr<const IStateProvider>
make_mission(CartesianState initial, MissionPlan plan, CoastFactory coast);
} // namespace orbitforge::core
