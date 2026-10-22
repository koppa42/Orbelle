#include <cmath>
#include <orbelle/core/frames/frame_provider.hpp>
#include <stdexcept>

namespace orbelle::core {
namespace {
std::array<double, 3> apply(const PositionRotation &rotation, const std::array<double, 3> &position,
                            bool transpose) {
    for (const auto value : position) {
        if (!std::isfinite(value)) {
            throw std::invalid_argument("Position components must be finite metres");
        }
    }
    std::array<double, 3> result{};
    for (std::size_t row = 0; row < 3; ++row) {
        for (std::size_t col = 0; col < 3; ++col) {
            result[row] +=
                (transpose ? rotation.matrix[col][row] : rotation.matrix[row][col]) * position[col];
        }
    }
    return result;
}
} // namespace
std::array<double, 3>
PositionRotation::apply_position(const std::array<double, 3> &position_m) const {
    return apply(*this, position_m, false);
}
std::array<double, 3>
PositionRotation::inverse_position(const std::array<double, 3> &position_m) const {
    return apply(*this, position_m, true);
}
} // namespace orbelle::core
