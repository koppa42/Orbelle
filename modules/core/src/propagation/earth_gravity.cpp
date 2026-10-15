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
using Coefficient = GravityCoefficient;
// Fully normalized coefficients. Generated from the pinned ICGEM EGM96 source;
// see tools/data/import_egm96.py and docs/architecture/propagators.md.
#include "egm96_coefficients.inc"
std::size_t index(int n, int m) { return static_cast<std::size_t>(n * (n + 1) / 2 + m); }
} // namespace
std::string_view egm96_model_id() noexcept { return "egm96-static-tide-free-20-v1"; }
std::shared_ptr<const GravityModel> builtin_egm96() {
    static const auto model = std::make_shared<const GravityModel>(
        GravityInfo{"EGM96 (built-in 20x20)",
                    "5247a9e9c316dd2c8f8fd491d53be0e163cb5cb3676b021754240cc9e44cb43b",
                    {},
                    "tide_free",
                    egm96_mu,
                    egm96_radius_m,
                    20,
                    20},
        std::vector<GravityCoefficient>(std::begin(egm96_coefficients),
                                        std::end(egm96_coefficients)));
    return model;
}
GravityModel::GravityModel(GravityInfo info, const std::vector<GravityCoefficient> &coefficients)
    : info_(std::move(info)) {
    if (!std::isfinite(info_.mu) || info_.mu <= 0 || !std::isfinite(info_.radius_m) ||
        info_.radius_m <= 0 || info_.degree < 2 || info_.degree > 2190 || info_.order < 0 ||
        info_.order > info_.degree || info_.model.empty() ||
        (info_.tide_system != "tide_free" && info_.tide_system != "zero_tide" &&
         info_.tide_system != "mean_tide"))
        throw std::invalid_argument("Invalid static Earth gravity metadata");
    const int degree = std::min(info_.degree, gravity_max_degree);
    terms_.resize(index(degree + 1, 0));
    std::vector<bool> seen(terms_.size());
    terms_[0].c = 1;
    for (const auto &v : coefficients) {
        if (v.degree < 0 || v.degree > info_.degree || v.order < 0 ||
            v.order > std::min(v.degree, info_.order) || !std::isfinite(v.c) || !std::isfinite(v.s))
            throw std::invalid_argument("Invalid gravity coefficient");
        if (v.degree > degree)
            continue;
        const auto i = index(v.degree, v.order);
        if (seen[i] || (v.degree == 0 && (v.c != 1 || v.s != 0)) ||
            (v.degree == 1 && (v.c != 0 || v.s != 0)) || (v.order == 0 && v.s != 0))
            throw std::invalid_argument("Duplicate or non-geocentric gravity coefficient");
        seen[i] = true;
        terms_[i].c = v.c;
        terms_[i].s = v.s;
    }
    for (int m = 0; m <= degree; ++m)
        for (int n = m + 1; n <= degree; ++n) {
            auto &term = terms_[index(n, m)];
            term.a = std::sqrt((4.0 * n * n - 1) / (n * n - m * m));
            if (n >= m + 2)
                term.b = std::sqrt((2.0 * n + 1) * ((n - 1.0) * (n - 1) - m * m) /
                                   ((2.0 * n - 3) * (n * n - m * m)));
        }
}
std::array<double, 3>
GravityModel::acceleration(const std::array<double, 3> &r, int degree, int order,
                           std::span<const GravityCoefficient> corrections) const {
    if (degree < 0 || degree == 1 || degree > std::min(info_.degree, gravity_max_degree) ||
        order < 0 || order > std::min(degree, info_.order))
        throw std::invalid_argument("Gravity truncation exceeds the model or computation limit");
    int correction_degree = 0, correction_order = 0;
    for (const auto &v : corrections) {
        if (v.degree < 2 || v.degree > gravity_max_degree || v.order < 0 || v.order > v.degree ||
            !std::isfinite(v.c) || !std::isfinite(v.s) || (v.order == 0 && v.s != 0))
            throw std::invalid_argument("Invalid additive gravity coefficient");
        correction_degree = std::max(correction_degree, v.degree);
        correction_order = std::max(correction_order, v.order);
    }
    struct Delta {
        double c = 0, s = 0;
        bool seen = false;
    };
    std::vector<Delta> delta(corrections.empty() ? 0 : index(correction_degree + 1, 0));
    for (const auto &v : corrections) {
        auto &d = delta[index(v.degree, v.order)];
        if (d.seen)
            throw std::invalid_argument("Duplicate additive gravity coefficient");
        d = {v.c, v.s, true};
    }
    const int effective_degree = std::max(degree, correction_degree);
    const int effective_order = std::max(order, correction_order);
    const double radius = std::hypot(r[0], r[1], r[2]);
    if (!std::isfinite(radius) || radius <= 0)
        throw PropagationError("Gravity requires a finite, nonzero position");
    const double inverse2 = 1 / (radius * radius);
    if (effective_degree == 0) {
        const double factor = -info_.mu * inverse2 / radius;
        return {factor * r[0], factor * r[1], factor * r[2]};
    }
    Jet inv2{inverse2}, invr{1 / radius};
    for (std::size_t i = 0; i < 3; ++i) {
        inv2.gradient[i] = -2 * r[i] * inverse2 * inverse2;
        invr.gradient[i] = -r[i] * inverse2 / radius;
    }
    const auto x = info_.radius_m * (Jet{r[0], {1, 0, 0}} * inv2);
    const auto y = info_.radius_m * (Jet{r[1], {0, 1, 0}} * inv2);
    const auto z = info_.radius_m * (Jet{r[2], {0, 0, 1}} * inv2);
    const auto rho = (info_.radius_m * info_.radius_m) * inv2;
    Jet diagonal_v = info_.radius_m * invr, diagonal_w{}, potential{};
    // Fully normalized Cartesian solid harmonics. O(1) recurrence workspace;
    // no factorials or unnormalized intermediate values at high degree/poles.
    for (int m = 0; m <= effective_order; ++m) {
        if (m > 0) {
            const double scale = m == 1 ? std::sqrt(3.0) : std::sqrt((2.0 * m + 1) / (2.0 * m));
            const auto next_v = scale * (x * diagonal_v - y * diagonal_w);
            diagonal_w = scale * (x * diagonal_w + y * diagonal_v);
            diagonal_v = next_v;
        }
        Jet v = diagonal_v, w = diagonal_w, previous_v{}, previous_w{};
        for (int n = m; n <= effective_degree; ++n) {
            const auto i = index(n, m);
            Term term{};
            if (i < terms_.size())
                term = terms_[i];
            else if (n > m) {
                // Corrections may extend beyond the base file's degree/order.
                term.a = std::sqrt((4.0 * n * n - 1) / (n * n - m * m));
                if (n >= m + 2)
                    term.b = std::sqrt((2.0 * n + 1) * ((n - 1.0) * (n - 1) - m * m) /
                                       ((2.0 * n - 3) * (n * n - m * m)));
            }
            if (n > m) {
                const auto next_v = term.a * (z * v) - term.b * (rho * previous_v);
                const auto next_w = term.a * (z * w) - term.b * (rho * previous_w);
                previous_v = v;
                previous_w = w;
                v = next_v;
                w = next_w;
            }
            double c = n <= degree && m <= order ? term.c : 0;
            double s = n <= degree && m <= order ? term.s : 0;
            if (i < delta.size()) {
                c += delta[i].c;
                s += delta[i].s;
            }
            potential = potential + c * v + s * w;
        }
    }
    auto acceleration = potential.gradient;
    for (auto &component : acceleration) {
        component *= info_.mu / info_.radius_m;
        if (!std::isfinite(component))
            throw PropagationError("Gravity acceleration is not finite at this position");
    }
    return acceleration;
}
std::array<double, 3> egm96_acceleration(const std::array<double, 3> &r, int degree, int order) {
    return builtin_egm96()->acceleration(r, degree, order);
}
} // namespace orbitforge::core
