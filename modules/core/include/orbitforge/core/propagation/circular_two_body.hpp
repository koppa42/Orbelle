#pragma once
#include <orbitforge/core/propagation/state_provider.hpp>
#include <orbitforge/core/units/quantities.hpp>

namespace orbitforge::core {
// P0 analytical fixture: an Earth-centred circular orbit in the GCRF XY plane.
// At the reference epoch position is +X and velocity is +Y.
// This is not the general Keplerian/J2/SGP4 propagator planned for P1.
class CircularTwoBody final : public IStateProvider {
  public:
    CircularTwoBody(Length radius, GravitationalParameter mu, Epoch reference_epoch);
    [[nodiscard]] CartesianState state_at(Epoch epoch) const override;

  private:
    double radius_m_;
    double speed_mps_;
    double angular_rate_radps_;
    Epoch reference_epoch_;
};
} // namespace orbitforge::core
