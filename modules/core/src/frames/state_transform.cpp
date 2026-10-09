#include <cmath>
#include <orbitforge/core/frames/frame_provider.hpp>
#include <stdexcept>

namespace orbitforge::core {
namespace {
void validate(const CartesianState &state, const PositionRotation &rotation, FrameId expected) {
    if (state.epoch != rotation.epoch || state.origin != rotation.origin ||
        state.frame != expected) {
        throw std::invalid_argument("State epoch, origin and frame must match the transformation");
    }
    for (std::size_t i = 0; i < 3; ++i) {
        if (!std::isfinite(state.position_m[i]) || !std::isfinite(state.velocity_mps[i])) {
            throw std::invalid_argument("State position and velocity must be finite SI values");
        }
    }
}
std::array<double, 3> rate_term(const Matrix3 &rate, const std::array<double, 3> &position) {
    std::array<double, 3> result{};
    for (std::size_t i = 0; i < 3; ++i) {
        for (std::size_t j = 0; j < 3; ++j) {
            result[i] += rate[i][j] * position[j];
        }
    }
    return result;
}
} // namespace

CartesianState StateTransform::apply(const CartesianState &state) const {
    validate(state, rotation, rotation.from);
    auto position = rotation.apply_position(state.position_m);
    auto velocity = rotation.apply_position(state.velocity_mps);
    const auto rate = rate_term(rotation_rate_per_second, state.position_m);
    for (std::size_t i = 0; i < 3; ++i) {
        velocity[i] += rate[i];
    }
    return {state.epoch, state.origin, rotation.to, position, velocity};
}
CartesianState StateTransform::inverse(const CartesianState &state) const {
    validate(state, rotation, rotation.to);
    auto position = rotation.inverse_position(state.position_m);
    auto velocity = state.velocity_mps;
    const auto rate = rate_term(rotation_rate_per_second, position);
    for (std::size_t i = 0; i < 3; ++i) {
        velocity[i] -= rate[i];
    }
    return {state.epoch, state.origin, rotation.from, position,
            rotation.inverse_position(velocity)};
}
} // namespace orbitforge::core
