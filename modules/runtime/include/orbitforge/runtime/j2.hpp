#pragma once
#include <orbitforge/core/propagation/central_body.hpp>
#include <orbitforge/core/propagation/j2_mean_elements.hpp>
#include <orbitforge/runtime/two_body.hpp>

namespace orbitforge::runtime {
// Auxiliary Keplerian position/velocity of a mean ellipse, NOT the derivative
// of the precessing trajectory. The conversion does not remove periodic terms.
[[nodiscard]] core::CartesianState
j2_mean_ellipse_state(core::J2MeanElements elements, core::OriginId body = core::OriginId::earth);
[[nodiscard]] core::J2MeanElements j2_mean_ellipse_elements(core::CartesianState auxiliary_state);
// Output velocity IS the derivative of the secular trajectory, including precession.
[[nodiscard]] application::Session
make_j2_session(core::J2MeanElements initial, core::J2SecularOptions options, std::string object_id,
                std::uint64_t input_revision = 1, core::OriginId body = core::OriginId::earth);
} // namespace orbitforge::runtime
