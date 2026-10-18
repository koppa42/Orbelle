#include <algorithm>
#include <cmath>
#include <limits>
#include <orbitforge/core/propagation/hpop_covariance.hpp>

namespace orbitforge::core {
namespace {
using Vector = std::array<double, 3>;
struct Axes {
    Matrix3 rotation{}, rate{};
};
double dot(const Vector &a, const Vector &b) {
    return std::fma(a[0], b[0], std::fma(a[1], b[1], a[2] * b[2]));
}
Vector cross(const Vector &a, const Vector &b) {
    return {a[1] * b[2] - a[2] * b[1], a[2] * b[0] - a[0] * b[2], a[0] * b[1] - a[1] * b[0]};
}
void finite(const Vector &v) {
    for (double x : v)
        if (!std::isfinite(x))
            throw std::invalid_argument("Covariance frame reference must be finite");
}
Vector divide(const Vector &v, double d) { return {v[0] / d, v[1] / d, v[2] / d}; }
Vector transverse(const Vector &v, const Vector &unit) {
    const auto parallel = dot(v, unit);
    return {std::fma(-parallel, unit[0], v[0]), std::fma(-parallel, unit[1], v[1]),
            std::fma(-parallel, unit[2], v[2])};
}
double norm(const Vector &v) { return std::hypot(v[0], v[1], v[2]); }
void coordinates(HpopCovarianceCoordinates value) {
    if (value.frame != HpopCovarianceFrame::gcrf && value.frame != HpopCovarianceFrame::lvlh &&
        value.frame != HpopCovarianceFrame::frenet)
        throw std::invalid_argument("Unsupported HPOP covariance frame");
    if (value.velocity != HpopCovarianceVelocity::projected &&
        value.velocity != HpopCovarianceVelocity::rotating)
        throw std::invalid_argument("Unsupported HPOP covariance velocity convention");
}
Axes axes(const CartesianState &reference, HpopCovarianceCoordinates coordinate,
          const std::optional<Vector> &acceleration) {
    Axes result;
    if (coordinate.frame == HpopCovarianceFrame::gcrf) {
        for (std::size_t i = 0; i < 3; ++i)
            result.rotation[i][i] = 1;
        return result;
    }
    const auto r = norm(reference.position_m), v = norm(reference.velocity_mps);
    if (!(r > 0) || !(v > 0) || !std::isfinite(r) || !std::isfinite(v))
        throw std::invalid_argument(
            "Local covariance axes need finite nonzero position and velocity");
    const auto radial = divide(reference.position_m, r),
               tangent = divide(reference.velocity_mps, v);
    const auto h = cross(radial, tangent);
    const double sine = norm(h);
    if (!(sine > 64 * std::numeric_limits<double>::epsilon()))
        throw std::invalid_argument("Local covariance axes are undefined for radial motion");
    const auto normal = divide(h, sine);
    const bool lvlh = coordinate.frame == HpopCovarianceFrame::lvlh;
    result.rotation = lvlh ? Matrix3{radial, cross(normal, radial), normal}
                           : Matrix3{cross(tangent, normal), tangent, normal};
    if (coordinate.velocity == HpopCovarianceVelocity::projected)
        return result;
    if (!acceleration)
        throw std::invalid_argument(
            "Rotating covariance axes require nominal inertial acceleration");
    const auto a_over_v = divide(*acceleration, v);
    const auto normal_rate = transverse(divide(cross(radial, a_over_v), sine), normal);
    const auto first_rate =
        lvlh ? divide(transverse(tangent, radial), r / v) : transverse(a_over_v, tangent);
    const auto left = lvlh ? cross(normal_rate, radial) : cross(first_rate, normal);
    const auto right = lvlh ? cross(normal, first_rate) : cross(tangent, normal_rate);
    Vector other_rate{};
    for (std::size_t i = 0; i < 3; ++i)
        other_rate[i] = left[i] + right[i];
    result.rate = lvlh ? Matrix3{first_rate, other_rate, normal_rate}
                       : Matrix3{other_rate, first_rate, normal_rate};
    // Enforce the rotational tangent identity Rdot R^T + R Rdot^T = 0 at
    // roundoff, without suppressing real cross-track acceleration/precession.
    Matrix3 omega{};
    for (std::size_t i = 0; i < 3; ++i)
        for (std::size_t j = 0; j < i; ++j) {
            omega[i][j] = .5 * (dot(result.rate[i], result.rotation[j]) -
                                dot(result.rate[j], result.rotation[i]));
            omega[j][i] = -omega[i][j];
        }
    for (std::size_t i = 0; i < 3; ++i)
        for (std::size_t j = 0; j < 3; ++j) {
            result.rate[i][j] = 0;
            for (std::size_t k = 0; k < 3; ++k)
                result.rate[i][j] = std::fma(omega[i][k], result.rotation[k][j], result.rate[i][j]);
        }
    return result;
}
} // namespace
HpopStateCovariance
hpop_covariance_frame_jacobian(const CartesianState &reference, HpopCovarianceCoordinates from,
                               HpopCovarianceCoordinates to,
                               std::optional<std::array<double, 3>> acceleration_mps2) {
    coordinates(from);
    coordinates(to);
    if (reference.origin != OriginId::earth || reference.frame != FrameId::gcrf)
        throw std::invalid_argument("Covariance frame reference must be Earth/GCRF");
    finite(reference.position_m);
    finite(reference.velocity_mps);
    if (acceleration_mps2)
        finite(*acceleration_mps2);
    const auto source = axes(reference, from, acceleration_mps2);
    const auto target = axes(reference, to, acceleration_mps2);
    HpopStateCovariance result{};
    Matrix3 rotation{}, source_spin{};
    for (std::size_t i = 0; i < 3; ++i)
        for (std::size_t j = 0; j < 3; ++j) {
            rotation[i][j] = dot(target.rotation[i], source.rotation[j]);
            source_spin[i][j] = dot(source.rate[i], source.rotation[j]);
        }
    for (std::size_t i = 0; i < 3; ++i)
        for (std::size_t j = 0; j < 3; ++j) {
            result[i][j] = result[i + 3][j + 3] = rotation[i][j];
            double rate = dot(target.rate[i], source.rotation[j]);
            for (std::size_t k = 0; k < 3; ++k)
                rate = std::fma(-rotation[i][k], source_spin[k][j], rate);
            result[i + 3][j] = rate;
        }
    for (const auto &row : result)
        for (double entry : row)
            if (!std::isfinite(entry))
                throw PropagationError(
                    "Covariance frame Jacobian is outside the finite double range");
    if (from == to || (from.frame == HpopCovarianceFrame::gcrf && to.frame == from.frame)) {
        result = {};
        for (std::size_t i = 0; i < 6; ++i)
            result[i][i] = 1;
    }
    return result;
}
} // namespace orbitforge::core
