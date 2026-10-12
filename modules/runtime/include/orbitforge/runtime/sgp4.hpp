#pragma once
#include <orbitforge/application/session.hpp>
#include <orbitforge/core/propagation/tle.hpp>

namespace orbitforge::runtime {
// WGS72 / improved mode, output TEME in SI, elapsed TAI. No downloads or element-set switching.
[[nodiscard]] application::Session make_sgp4_session(const core::Tle &tle,
                                                     const core::ITimeProvider &time,
                                                     std::string object_id,
                                                     std::uint64_t input_revision = 1);
} // namespace orbitforge::runtime
