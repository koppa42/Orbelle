#include <cmath>
#include <orbitforge/core/propagation/third_body.hpp>
#include <stdexcept>

namespace orbitforge::core {
std::array<double, 3> third_body_acceleration(const std::array<double, 3> &r,
                                              const std::array<double, 3> &rb, double mu) {
    if (!std::isfinite(mu) || mu <= 0)
        throw std::invalid_argument("Third-body GM must be finite and positive");
    const double distance = std::hypot(rb[0], rb[1], rb[2]);
    if (!(distance > 0) || !std::isfinite(distance))
        throw std::invalid_argument("Third-body position must be finite and nonzero");
    std::array<double, 3> delta{}, u{}, v{};
    for (int i = 0; i < 3; ++i) {
        if (!std::isfinite(r[i]) || !std::isfinite(rb[i]))
            throw std::invalid_argument("Third-body positions must be finite");
        delta[i] = r[i] - rb[i];
        u[i] = r[i] / distance;
        v[i] = rb[i] / distance;
    }
    const double separation = std::hypot(delta[0], delta[1], delta[2]);
    if (!(distance > 0) || !std::isfinite(distance) || !(separation > 0) ||
        !std::isfinite(separation))
        throw std::invalid_argument("Third-body gravity is singular at a body centre");
    // q = |r-rb|^2/|rb|^2 - 1, without subtracting two nearly equal distances.
    double q = 0;
    for (int i = 0; i < 3; ++i)
        q += u[i] * (u[i] - 2 * v[i]);
    // f = (1+q)^(3/2)-1, rationalized to preserve the tidal term near r=0.
    const double ratio = separation / distance;
    const double cube = ratio * ratio * ratio;
    const double f = q * (3 + q * (3 + q)) / (1 + cube);
    const double scale = -mu / separation / separation / separation;
    std::array<double, 3> result{};
    for (int i = 0; i < 3; ++i) {
        result[i] = scale * (r[i] + f * rb[i]);
        if (!std::isfinite(result[i]))
            throw std::invalid_argument("Third-body acceleration is not finite");
    }
    return result;
}
} // namespace orbitforge::core
