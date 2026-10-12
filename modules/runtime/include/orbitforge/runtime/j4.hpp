#pragma once
#include <orbitforge/application/session.hpp>
#include <orbitforge/core/propagation/j4_mean_elements.hpp>

namespace orbitforge::runtime {
// Auxiliary mean ellipse under the EGM2008 constants; not an osculating-element conversion.
[[nodiscard]] core::CartesianState j4_mean_ellipse_state(core::J4MeanElements elements);
[[nodiscard]] core::J4MeanElements j4_mean_ellipse_elements(core::CartesianState auxiliary_state);
// Fixed a,e,i; secular J2, J2^2 and J4 in GCRF. Velocity differentiates the full trajectory.
[[nodiscard]] application::Session make_j4_session(core::J4MeanElements initial,
                                                   std::string object_id,
                                                   std::uint64_t input_revision = 1);
} // namespace orbitforge::runtime
