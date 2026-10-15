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
// Schmidt-normalized angular recurrence, with sqrt(2n+1) applied on accumulation.
// Its usual first coefficient is 1 + b + offset. Evaluate the small offset by
// rationalizing n-sqrt(n*n-m*m), so the zonal offset is exactly zero at the poles.
std::pair<double, double> recurrence(int n, int m) {
    const double root = std::sqrt(static_cast<double>(n) * n - m * m);
    const double previous = std::sqrt((n - 1.0) * (n - 1) - m * m);
    const double offset =
        m == 0 ? 0
               : (static_cast<double>(m) * m / root) * (1 / (n + root) + 1 / (n - 1 + previous));
    return {offset, previous / root};
}
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
        info_.radius_m <= 0 || info_.degree < 2 || info_.degree > gravity_max_degree ||
        info_.order < 0 || info_.order > info_.degree || info_.model.empty() ||
        !std::isfinite(info_.solid_tide_k20) ||
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
            const auto [offset, b] = recurrence(n, m);
            term.offset = offset;
            term.b = b;
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
    const double inverse_radius = 1 / radius;
    const std::array<double, 3> direction{r[0] * inverse_radius, r[1] * inverse_radius,
                                          r[2] * inverse_radius};
    Jet x{direction[0]}, y{direction[1]}, u{std::abs(direction[2])};
    for (std::size_t i = 0; i < 3; ++i) {
        x.gradient[i] = ((i == 0 ? 1 : 0) - direction[0] * direction[i]) * inverse_radius;
        y.gradient[i] = ((i == 1 ? 1 : 0) - direction[1] * direction[i]) * inverse_radius;
        u.gradient[i] = -u.value * direction[i] * inverse_radius;
    }
    const double transverse2 = direction[0] * direction[0] + direction[1] * direction[1];
    u.gradient[2] = (r[2] < 0 ? -1 : 1) * transverse2 * inverse_radius;
    Jet u_minus_one = u;
    u_minus_one.value = -transverse2 / (1 + u.value);
    const bool polar = u.value > .75;
    // Keep the radial powers out of the angular recurrence. This avoids repeated
    // cancellation of nearly equal radial terms at high degree near the poles.
    std::vector<double> weights(static_cast<std::size_t>(effective_degree + 1));
    const double ratio = info_.radius_m * inverse_radius;
    const double log_ratio = std::abs(radius - info_.radius_m) < .5 * info_.radius_m
                                 ? std::log1p((info_.radius_m - radius) * inverse_radius)
                                 : std::log(ratio);
    for (int n = 0; n <= effective_degree; ++n)
        weights[static_cast<std::size_t>(n)] =
            ratio * std::exp(n * log_ratio) * std::sqrt(2.0 * n + 1);
    Jet diagonal_v{1}, diagonal_w{};
    std::array<double, 3> acceleration{}, compensation{};
    // Cartesian angular harmonics avoid longitude divisions, including x=y=0.
    // Two previous degrees suffice; the radial weights use O(degree) workspace.
    for (int m = 0; m <= effective_order; ++m) {
        if (m > 0) {
            const double scale = m == 1 ? 1 : std::sqrt((2.0 * m - 1) / (2.0 * m));
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
                const auto [offset, b] = recurrence(n, m);
                term.offset = offset;
                term.b = b;
            }
            if (n > m) {
                const double a = 1 + term.b + term.offset;
                const auto next = [&](const Jet &current, const Jet &previous) {
                    if (polar)
                        return current + term.b * (current - previous) +
                               (Jet{term.offset} + a * u_minus_one) * current;
                    return a * (u * current) - term.b * previous;
                };
                const auto next_v = next(v, previous_v);
                const auto next_w = next(w, previous_w);
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
            if (c == 0 && s == 0)
                continue;
            const auto angular = c * v + s * w;
            const double weight =
                weights[static_cast<std::size_t>(n)] * (r[2] < 0 && (n - m) % 2 ? -1 : 1);
            for (std::size_t axis = 0; axis < 3; ++axis) {
                const double contribution =
                    weight * (angular.gradient[axis] -
                              (n + 1) * inverse_radius * direction[axis] * angular.value);
                // Compensate summation across millions of normalized coefficients.
                const double adjusted = contribution - compensation[axis];
                const double sum = acceleration[axis] + adjusted;
                compensation[axis] = (sum - acceleration[axis]) - adjusted;
                acceleration[axis] = sum;
            }
        }
    }
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
