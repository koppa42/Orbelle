#pragma once
#include <memory>
#include <orbitforge/core/propagation/state_provider.hpp>
#include <orbitforge/core/propagation/tle.hpp>

namespace orbitforge::providers::sgp4 {
// Vallado SGP4, WGS72, improved mode; includes the deep-space branch.
// Native output is Earth-centred TEME in SI units. Queries use elapsed TAI minutes.
// time is used only to interpret the TLE epoch; the returned provider owns immutable input.
[[nodiscard]] std::shared_ptr<const core::IStateProvider>
make_sgp4(const core::Tle &tle, const core::ITimeProvider &time);
} // namespace orbitforge::providers::sgp4
