#include <algorithm>
#include <cmath>
#include <orbitforge/core/propagation/hpop.hpp>

namespace orbitforge::core {
namespace {
// Cartesian solid harmonics with first derivatives. Differentiating the recurrence
// avoids longitude/latitude divisions (including at x=y=0), and finite differences.
struct Jet {
    double value = 0;
    std::array<double, 3> gradient{};
};
Jet operator+(const Jet &a, const Jet &b) {
    Jet r{a.value + b.value};
    for (std::size_t i = 0; i < 3; ++i)
        r.gradient[i] = a.gradient[i] + b.gradient[i];
    return r;
}
Jet operator-(const Jet &a, const Jet &b) {
    Jet r{a.value - b.value};
    for (std::size_t i = 0; i < 3; ++i)
        r.gradient[i] = a.gradient[i] - b.gradient[i];
    return r;
}
Jet operator*(const Jet &a, const Jet &b) {
    Jet r{a.value * b.value};
    for (std::size_t i = 0; i < 3; ++i)
        r.gradient[i] = a.gradient[i] * b.value + a.value * b.gradient[i];
    return r;
}
Jet operator*(double a, const Jet &b) { return Jet{a} * b; }
struct Coefficient {
    int degree, order;
    double c, s;
};
// Fully normalized coefficients. Generated from the pinned ICGEM EGM96 source;
// see tools/data/import_egm96.py and docs/architecture/propagators.md.
#include "egm96_coefficients.inc"
using Coefficients = std::array<std::array<std::array<double, 2>, 21>, 21>;
const Coefficients &unnormalized() {
    static const auto values = [] {
        Coefficients result{};
        result[0][0][0] = 1;
        for (const auto &v : egm96_coefficients) {
            // Pbar_nm = sqrt((2-delta_m0)(2n+1)(n-m)!/(n+m)!) P_nm.
            double ratio = 1;
            for (int k = v.degree - v.order + 1; k <= v.degree + v.order; ++k)
                ratio /= k;
            const double scale = std::sqrt((v.order == 0 ? 1 : 2) * (2 * v.degree + 1) * ratio);
            result[v.degree][v.order] = {scale * v.c, scale * v.s};
        }
        return result;
    }();
    return values;
}
} // namespace
std::string_view egm96_model_id() noexcept { return "egm96-static-tide-free-20-v1"; }
std::array<double, 3> egm96_acceleration(const std::array<double, 3> &r, int degree, int order) {
    if (degree < 0 || degree == 1 || degree > egm96_max_degree || order < 0 || order > degree)
        throw std::invalid_argument("EGM96 requires degree 0 or 2..20 and order 0..degree");
    const double radius = std::hypot(r[0], r[1], r[2]);
    if (!std::isfinite(radius) || radius <= 0)
        throw PropagationError("Gravity requires a finite, nonzero position");
    const double inverse2 = 1 / (radius * radius);
    if (degree == 0) {
        const double factor = -egm96_mu * inverse2 / radius;
        return {factor * r[0], factor * r[1], factor * r[2]};
    }
    Jet inv2{inverse2}, invr{1 / radius};
    for (std::size_t i = 0; i < 3; ++i) {
        inv2.gradient[i] = -2 * r[i] * inverse2 * inverse2;
        invr.gradient[i] = -r[i] * inverse2 / radius;
    }
    const auto x = egm96_radius_m * (Jet{r[0], {1, 0, 0}} * inv2);
    const auto y = egm96_radius_m * (Jet{r[1], {0, 1, 0}} * inv2);
    const auto z = egm96_radius_m * (Jet{r[2], {0, 0, 1}} * inv2);
    const auto rho = (egm96_radius_m * egm96_radius_m) * inv2;
    std::array<std::array<Jet, 21>, 21> v{}, w{};
    v[0][0] = egm96_radius_m * invr;
    for (int m = 0; m <= order; ++m) {
        if (m > 0) {
            v[m][m] = (2.0 * m - 1) * (x * v[m - 1][m - 1] - y * w[m - 1][m - 1]);
            w[m][m] = (2.0 * m - 1) * (x * w[m - 1][m - 1] + y * v[m - 1][m - 1]);
        }
        for (int n = m + 1; n <= degree; ++n) {
            const double a = (2.0 * n - 1) / (n - m);
            v[n][m] = a * (z * v[n - 1][m]);
            w[n][m] = a * (z * w[n - 1][m]);
            if (n >= m + 2) {
                const double b = (n + m - 1.0) / (n - m);
                v[n][m] = v[n][m] - b * (rho * v[n - 2][m]);
                w[n][m] = w[n][m] - b * (rho * w[n - 2][m]);
            }
        }
    }
    Jet potential = v[0][0];
    const auto &coefficients = unnormalized();
    for (int n = 2; n <= degree; ++n)
        for (int m = 0; m <= std::min(n, order); ++m)
            potential =
                potential + coefficients[n][m][0] * v[n][m] + coefficients[n][m][1] * w[n][m];
    auto acceleration = potential.gradient;
    for (auto &component : acceleration)
        component *= egm96_mu / egm96_radius_m;
    return acceleration;
}
} // namespace orbitforge::core
