#pragma once
#include <filesystem>
#include <memory>
#include <orbitforge/core/ephemeris/ephemeris_provider.hpp>
#include <orbitforge/core/propagation/keplerian_elements.hpp>
#include <orbitforge/core/propagation/state_provider.hpp>
#include <orbitforge/core/units/quantities.hpp>

namespace orbitforge::providers::spice {
// The caller supplies a writable workspace for owned temporary kernel copies.
// All CSPICE access to this linked instance must use this wrapper.
[[nodiscard]] std::unique_ptr<core::IEphemerisProvider>
load_snapshot(const std::filesystem::path &manifest, const std::filesystem::path &workspace);

// Kernel-free ideal two-body dynamics. SI values and elapsed TAI seconds;
// shares the same CSPICE global-state lock as ephemeris queries.
[[nodiscard]] std::shared_ptr<const core::IStateProvider>
make_two_body(core::CartesianState initial, core::GravitationalParameter mu);
[[nodiscard]] core::CartesianState to_cartesian(core::KeplerianElements elements,
                                                core::GravitationalParameter mu);
[[nodiscard]] core::KeplerianElements to_keplerian(core::CartesianState state,
                                                   core::GravitationalParameter mu);
} // namespace orbitforge::providers::spice
