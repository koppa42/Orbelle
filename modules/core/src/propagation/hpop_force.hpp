#pragma once
#include <orbitforge/core/propagation/hpop_force.hpp>

namespace orbitforge::core::detail {
// Shared preparation for a standalone force query and a propagation initial state.
// Resolves the existing built-in gravity choice into the caller's private copy.
void prepare_hpop_force_environment(const CartesianState &state, const HpopOptions &options,
                                    HpopEnvironment &environment);
void validate_hpop_force_state(const std::array<double, 3> &position,
                               const std::array<double, 3> &velocity, const HpopOptions &options,
                               const HpopEnvironment &environment);
// Caller has prepared resources and certified the full propagation interval.
// Weather sampling and cylindrical lighting can use the current event-side limit.
[[nodiscard]] std::array<double, 3> hpop_acceleration_unchecked(
    const CartesianState &state, const HpopOptions &options, const HpopEnvironment &environment,
    std::optional<Epoch> weather_at = {}, std::optional<double> shadow_lighting = {});
[[nodiscard]] HpopForceDifferential hpop_differential_unchecked(
    const CartesianState &state, const HpopOptions &options, const HpopEnvironment &environment,
    std::optional<Epoch> weather_at = {}, const HpopOptions *differential_gravity = nullptr,
    const std::function<void()> &checkpoint = {},
    std::optional<bool> initial_drag_above_cutoff = {});
} // namespace orbitforge::core::detail
