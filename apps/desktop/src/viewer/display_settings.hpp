#pragma once
#include "orbit_scene.hpp"
#include <algorithm>
#include <cmath>
#include <numbers>

namespace orbitforge::desktop {
using DisplaySettings = application::OrbitDisplay;
inline std::pair<core::Epoch, core::Epoch>
display_interval(const OrbitInput &input, const DisplaySettings &settings, core::Epoch current) {
    const auto first = input.first.tai_nanoseconds(), last = input.last.tai_nanoseconds();
    if (settings.extent == 1)
        return {input.first, input.last};
    double past = settings.past_minutes * 60, future = settings.future_minutes * 60;
    if (settings.extent == 0) {
        const auto &state = input.initial;
        const double r = std::hypot(state.position_m[0], state.position_m[1], state.position_m[2]);
        const double v =
            std::hypot(state.velocity_mps[0], state.velocity_mps[1], state.velocity_mps[2]);
        const double reciprocal_a = 2 / r - v * v / input.mu();
        const double period =
            reciprocal_a > 0
                ? 2 * std::numbers::pi / std::sqrt(input.mu() * std::pow(reciprocal_a, 3))
                : 3600;
        const auto span = std::min(static_cast<double>(last - first), period * 1e9);
        const auto begin =
            std::clamp(current.tai_nanoseconds() - static_cast<std::int64_t>(span / 2), first,
                       last - static_cast<std::int64_t>(span));
        return {core::Epoch{begin}, core::Epoch{begin + static_cast<std::int64_t>(span)}};
    }
    if (settings.extent != 2 || !std::isfinite(past) || !std::isfinite(future) || past < 0 ||
        future < 0 || past + future <= 0 || past > 604800 || future > 604800)
        throw std::invalid_argument("Invalid display trail/lead interval");
    return {core::Epoch{
                std::max(first, current.tai_nanoseconds() - static_cast<std::int64_t>(past * 1e9))},
            core::Epoch{std::min(last, current.tai_nanoseconds() +
                                           static_cast<std::int64_t>(future * 1e9))}};
}
inline std::pair<core::Epoch, core::Epoch>
display_interval(const OrbitScene &orbit, const DisplaySettings &settings, core::Epoch current) {
    return display_interval(orbit.input, settings, current);
}
inline std::pair<core::Epoch, core::Epoch> display_cache_interval(const OrbitInput &input,
                                                                  const DisplaySettings &settings,
                                                                  core::Epoch current) {
    auto [first, last] = display_interval(input, settings, current);
    // A trailing-only interval at the scenario start (or leading-only at its end)
    // may be empty. Keep a small valid cache so the current point remains available.
    const auto padding =
        std::max<std::int64_t>(static_cast<std::int64_t>(input.step_seconds) * 1'000'000'000,
                               (last.tai_nanoseconds() - first.tai_nanoseconds()) * 15 / 100);
    return {core::Epoch{std::max(input.first.tai_nanoseconds(), first.tai_nanoseconds() - padding)},
            core::Epoch{std::min(input.last.tai_nanoseconds(), last.tai_nanoseconds() + padding)}};
}
} // namespace orbitforge::desktop
