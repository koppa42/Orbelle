#include <cmath>
#include <numbers>
#include <orbelle/core/propagation/circular_two_body.hpp>
#include <stdexcept>

namespace orbelle::core {
CircularTwoBody::CircularTwoBody(Length radius, GravitationalParameter mu, Epoch reference_epoch)
    : radius_m_(radius.metres), speed_mps_(0.0), angular_rate_radps_(0.0),
      reference_epoch_(reference_epoch) {
    if (!std::isfinite(radius_m_) || radius_m_ <= 0.0 ||
        !std::isfinite(mu.metres_cubed_per_second_squared) ||
        mu.metres_cubed_per_second_squared <= 0.0) {
        throw std::invalid_argument(
            "Radius and gravitational parameter must be finite and positive");
    }
    speed_mps_ = std::sqrt(mu.metres_cubed_per_second_squared / radius_m_);
    angular_rate_radps_ = speed_mps_ / radius_m_;
    if (!std::isfinite(speed_mps_) || !std::isfinite(angular_rate_radps_) || speed_mps_ <= 0.0 ||
        angular_rate_radps_ <= 0.0) {
        throw std::invalid_argument("Orbit parameters exceed the numerical range of this fixture");
    }
}
CartesianState CircularTwoBody::state_at(Epoch epoch) const {
    const auto phase = std::remainder(angular_rate_radps_ * epoch.seconds_since(reference_epoch_),
                                      2.0 * std::numbers::pi);
    const auto c = std::cos(phase);
    const auto s = std::sin(phase);
    return {epoch,
            OriginId::earth,
            FrameId::gcrf,
            {radius_m_ * c, radius_m_ * s, 0.0},
            {-speed_mps_ * s, speed_mps_ * c, 0.0}};
}
} // namespace orbelle::core
