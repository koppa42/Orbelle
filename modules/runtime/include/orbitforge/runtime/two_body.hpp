#pragma once
#include <cstdint>
#include <orbitforge/application/session.hpp>
#include <orbitforge/core/propagation/keplerian_elements.hpp>
#include <orbitforge/core/units/quantities.hpp>
#include <string>

namespace orbitforge::runtime {
[[nodiscard]] bool two_body_available() noexcept;
[[nodiscard]] core::CartesianState to_cartesian(core::KeplerianElements elements,
                                                core::GravitationalParameter mu);
[[nodiscard]] core::KeplerianElements to_keplerian(core::CartesianState state,
                                                   core::GravitationalParameter mu);

// SI, Earth-centred GCRF. Ideal two-body velocity is per continuous TAI second.
// Throws when CSPICE was disabled at build time. No ephemeris data files needed.
[[nodiscard]] application::Session make_two_body_session(core::CartesianState initial,
                                                         core::GravitationalParameter mu,
                                                         std::string object_id,
                                                         std::uint64_t input_revision = 1);
} // namespace orbitforge::runtime
