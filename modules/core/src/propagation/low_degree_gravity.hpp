#pragma once
#include <array>
#include <cmath>
#include <cstddef>

namespace orbelle::core::detail {
// The usual 20/21-degree field fits a small Cartesian solid-harmonic table.
// Its gradient uses neighbouring harmonics instead of propagating three first
// derivatives through every recurrence. Above this bound retain the existing
// O(degree) workspace and compensated near-pole recurrence for full EGM2008.
inline constexpr int direct_gravity_degree = 32;
inline constexpr std::size_t harmonic_index(int n, int m) {
    return static_cast<std::size_t>(n * (n + 1) / 2 + m);
}
struct HarmonicFactors {
    double a = 0, b = 0, diagonal = 0, plus = 0, minus = 0, vertical = 0;
};
inline const auto &harmonic_factors() {
    static const auto factors = [] {
        std::array<HarmonicFactors, harmonic_index(direct_gravity_degree + 3, 0)> result{};
        for (int n = 0; n <= direct_gravity_degree + 1; ++n) {
            for (int m = 0; m <= n; ++m) {
                auto &f = result[harmonic_index(n, m)];
                if (n > m) {
                    f.a = std::sqrt((2.0 * n + 1) * (2.0 * n - 1) / (n * n - m * m));
                    if (n > m + 1)
                        f.b = std::sqrt((2.0 * n + 1) / (2.0 * n - 3) *
                                        ((n - 1.0) * (n - 1) - m * m) / (n * n - m * m));
                } else if (n > 0) {
                    f.diagonal = n == 1 ? std::sqrt(3.) : std::sqrt((2.0 * n + 1) / (2.0 * n));
                }
                const double ratio = (2.0 * n + 1) / (2.0 * n + 3);
                f.plus = std::sqrt(ratio * (n + m + 1.0) * (n + m + 2) * (m == 0 ? .5 : 1));
                if (m > 0)
                    f.minus = std::sqrt(ratio * (n - m + 1.0) * (n - m + 2) * (m == 1 ? 2 : 1));
                f.vertical = std::sqrt(ratio * (n - m + 1.0) * (n + m + 1));
            }
        }
        return result;
    }();
    return factors;
}

template <class Coefficients>
std::array<double, 3> low_degree_acceleration(const std::array<double, 3> &position, double radius,
                                              double reference_radius, double mu, int degree,
                                              int order, Coefficients coefficients) {
    const auto &factors = harmonic_factors();
    // No longitude or transverse-radius divisions: the same expressions hold
    // at both poles, including sectorial and tesseral horizontal acceleration.
    std::array<std::array<double, 2>, harmonic_index(direct_gravity_degree + 3, 0)> values;
    const double ratio = reference_radius / radius;
    const double q = ratio * ratio;
    const double x = position[0] / radius * ratio, y = position[1] / radius * ratio,
                 z = position[2] / radius * ratio;
    values[0] = {ratio, 0};
    for (int m = 0; m <= order + 1; ++m) {
        if (m > 0) {
            const auto &previous = values[harmonic_index(m - 1, m - 1)];
            const double scale = factors[harmonic_index(m, m)].diagonal;
            values[harmonic_index(m, m)] = {scale * (x * previous[0] - y * previous[1]),
                                            scale * (x * previous[1] + y * previous[0])};
        }
        for (int n = m + 1; n <= degree + 1; ++n) {
            const auto i = harmonic_index(n, m);
            const auto &f = factors[i];
            const auto &previous = values[harmonic_index(n - 1, m)];
            for (std::size_t k = 0; k < 2; ++k)
                values[i][k] = f.a * z * previous[k] -
                               (n > m + 1 ? f.b * q * values[harmonic_index(n - 2, m)][k] : 0);
        }
    }
    std::array<double, 3> acceleration{}, compensation{};
    for (int m = 0; m <= order; ++m) {
        for (int n = m; n <= degree; ++n) {
            const auto [c, s] = coefficients(n, m);
            if (c == 0 && s == 0)
                continue;
            const auto &f = factors[harmonic_index(n, m)];
            const auto &up = values[harmonic_index(n + 1, m + 1)];
            const auto &same = values[harmonic_index(n + 1, m)];
            std::array<double, 3> term;
            if (m == 0) {
                term = {-c * f.plus * up[0], -c * f.plus * up[1], -c * f.vertical * same[0]};
            } else {
                const auto &down = values[harmonic_index(n + 1, m - 1)];
                term = {
                    (-f.plus * (c * up[0] + s * up[1]) + f.minus * (c * down[0] + s * down[1])) *
                        .5,
                    (-f.plus * (c * up[1] - s * up[0]) - f.minus * (c * down[1] - s * down[0])) *
                        .5,
                    -f.vertical * (c * same[0] + s * same[1])};
            }
            for (std::size_t axis = 0; axis < 3; ++axis) {
                const double adjusted = term[axis] - compensation[axis];
                const double sum = acceleration[axis] + adjusted;
                compensation[axis] = (sum - acceleration[axis]) - adjusted;
                acceleration[axis] = sum;
            }
        }
    }
    for (auto &component : acceleration)
        component *= mu / (reference_radius * reference_radius);
    return acceleration;
}
} // namespace orbelle::core::detail
