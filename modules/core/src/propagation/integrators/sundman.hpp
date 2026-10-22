#pragma once
#include <algorithm>
#include <array>
#include <cmath>
#include <limits>
#include <numbers>
#include <orbelle/core/propagation/state_provider.hpp>
#include <stdexcept>

namespace orbelle::core::detail {
// STK's dS = sqrt(gravitational_parameter) r^-n dt, with a constant nondimensionalization:
// q = S / a0^(3/2-n), dt/dq = sqrt(a0^3/gravitational_parameter) (r/a0)^n.
// a0 and e0 are the initial osculating ellipse. One unperturbed revolution spans
// period() in q; its fixed step is period()/steps_per_orbit. Forces always receive
// physical SI time and velocity. This transform does not choose/clip event times.
template <std::size_t Extra = 0> class SundmanSystemTransform {
  public:
    using Cartesian = std::array<double, 6 + Extra>;
    using FirstOrder = std::array<double, 7 + Extra>;   // r, v, physical seconds
    using SecondOrder = std::array<double, 7 + Extra>;  // r, dr/dq, physical seconds
    using Acceleration = std::array<double, 4 + Extra>; // d2r/dq2, dt/dq

    SundmanSystemTransform(const Cartesian &initial, double gravitational_parameter,
                           double exponent)
        : mu_(gravitational_parameter), exponent_(exponent) {
        if (!std::isfinite(gravitational_parameter) || gravitational_parameter <= 0 ||
            !std::isfinite(exponent) || exponent <= 0 || exponent > 10 || !finite(initial))
            throw std::invalid_argument(
                "Invalid Sundman gravitational parameter, exponent or state");
        const double r = std::hypot(initial[0], initial[1], initial[2]);
        const double v = std::hypot(initial[3], initial[4], initial[5]);
        if (!(r > 0))
            throw std::invalid_argument("Sundman initial radius must be positive");
        const double inverse_a = 2 / r - v * (v / gravitational_parameter);
        const std::array<double, 3> h{initial[1] * initial[5] - initial[2] * initial[4],
                                      initial[2] * initial[3] - initial[0] * initial[5],
                                      initial[0] * initial[4] - initial[1] * initial[3]};
        const std::array<double, 3> e{
            (initial[4] * h[2] - initial[5] * h[1]) / gravitational_parameter - initial[0] / r,
            (initial[5] * h[0] - initial[3] * h[2]) / gravitational_parameter - initial[1] / r,
            (initial[3] * h[1] - initial[4] * h[0]) / gravitational_parameter - initial[2] / r};
        eccentricity_ = std::hypot(e[0], e[1], e[2]);
        if (!(inverse_a > 0) || !(eccentricity_ < 1) || !(std::hypot(h[0], h[1], h[2]) > 0))
            throw std::invalid_argument(
                "Sundman steps per orbit require a nonradial bound initial orbit");
        a_ = 1 / inverse_a;
        time_scale_ = a_ / std::sqrt(gravitational_parameter / a_);
        period_ = kepler_period(exponent_, eccentricity_);
        if (!std::isfinite(a_) || !std::isfinite(time_scale_) || time_scale_ <= 0 ||
            !std::isfinite(period_) || period_ <= 0)
            throw std::invalid_argument("Sundman initial orbit has unrepresentable scales");
        (void)clock_rate(initial);
    }
    double semimajor_axis() const { return a_; }
    double eccentricity() const { return eccentricity_; }
    double time_scale() const { return time_scale_; }
    double period() const { return period_; }
    double exponent() const { return exponent_; }

    template <std::size_t N> double clock_rate(const std::array<double, N> &s) const {
        const double r = std::hypot(s[0], s[1], s[2]);
        const double value = time_scale_ * std::pow(r / a_, exponent_);
        if (!(r > 0) || !std::isfinite(value) || value <= 0)
            throw PropagationError("Sundman clock rate is not finite and positive");
        return value;
    }
    static FirstOrder first_order(const Cartesian &s, double time) {
        FirstOrder result{s[0], s[1], s[2], s[3], s[4], s[5], time};
        std::copy(s.begin() + 6, s.end(), result.begin() + 7);
        return result;
    }
    SecondOrder second_order(const Cartesian &s, double time) const {
        const double g = clock_rate(s);
        auto result = first_order(s, time);
        for (std::size_t i = 3; i < 6; ++i)
            result[i] *= g;
        if constexpr (Extra == 48)
            for (std::size_t i = 7 + 24; i < 7 + Extra; ++i)
                result[i] *= g;
        return result;
    }
    static Cartesian cartesian_first(const FirstOrder &s) {
        Cartesian result{s[0], s[1], s[2], s[3], s[4], s[5]};
        std::copy(s.begin() + 7, s.end(), result.begin() + 6);
        return result;
    }
    Cartesian cartesian_second(const SecondOrder &s) const {
        if (!finite(s))
            throw PropagationError("Sundman second-order state is not finite");
        const double g = clock_rate(s);
        auto result = cartesian_first(s);
        for (std::size_t i = 3; i < 6; ++i)
            result[i] /= g;
        if constexpr (Extra == 48)
            for (std::size_t i = 6 + 24; i < 6 + Extra; ++i)
                result[i] /= g;
        if (!finite(result))
            throw PropagationError("Sundman physical velocity is not finite");
        return result;
    }
    template <class Force> FirstOrder derivative(const FirstOrder &s, const Force &force) const {
        if (!finite(s))
            throw PropagationError("Sundman state is not finite");
        const double g = clock_rate(s);
        const auto acceleration = force(s[6], cartesian_first(s));
        FirstOrder result{s[3] * g,
                          s[4] * g,
                          s[5] * g,
                          acceleration[0] * g,
                          acceleration[1] * g,
                          acceleration[2] * g,
                          g};
        for (std::size_t i = 0; i < Extra; ++i)
            result[i + 7] = acceleration[i + 3] * g;
        return result;
    }
    template <class Force>
    Acceleration acceleration(const SecondOrder &s, const Force &force) const {
        const auto state = cartesian_second(s);
        return transform_acceleration(s, force(s[6], state));
    }
    // Retain the physical perturbing acceleration from the last evaluation;
    // reevaluate two-body acceleration and all coordinate/clock factors. Keeping
    // the old transformed perturbation would incorrectly freeze its (dt/dq)^2.
    Acceleration pseudo_correction(const SecondOrder &before, const SecondOrder &after,
                                   const Acceleration &previous) const {
        (void)cartesian_second(before);
        const auto state = cartesian_second(after);
        const double old_kinematic = kinematic(before), old_clock = clock_rate(before);
        const auto old_central = central(before), new_central = central(state);
        std::array<double, 3 + Extra> physical{};
        for (std::size_t i = 0; i < 3; ++i)
            physical[i] = (previous[i] - old_kinematic * before[i + 3]) / old_clock / old_clock -
                          old_central[i] + new_central[i];
        // The caller refreshes variational derivatives at the corrected state.
        return transform_acceleration(after, physical);
    }

  private:
    double mu_, exponent_, a_ = 0, eccentricity_ = 0, time_scale_ = 0, period_ = 0;
    template <std::size_t N> static bool finite(const std::array<double, N> &s) {
        return std::all_of(s.begin(), s.end(), [](double v) { return std::isfinite(v); });
    }
    template <std::size_t N> std::array<double, 3> central(const std::array<double, N> &s) const {
        const double r = std::hypot(s[0], s[1], s[2]);
        const double scale = -mu_ / r / r / r;
        return {scale * s[0], scale * s[1], scale * s[2]};
    }
    double kinematic(const SecondOrder &s) const {
        const double r = std::hypot(s[0], s[1], s[2]);
        if (!(r > 0))
            throw PropagationError("Sundman radius must be positive");
        double radial = 0;
        for (std::size_t i = 0; i < 3; ++i)
            radial += (s[i] / r) * (s[i + 3] / r);
        return exponent_ * radial;
    }
    Acceleration transform_acceleration(const SecondOrder &s,
                                        const std::array<double, 3 + Extra> &physical) const {
        const double k = kinematic(s), g = clock_rate(s);
        Acceleration result{physical[0] * g * g + k * s[3], physical[1] * g * g + k * s[4],
                            physical[2] * g * g + k * s[5], g};
        for (std::size_t i = 0; i < Extra; ++i)
            result[i + 4] = physical[i + 3] * g;
        if constexpr (Extra == 48)
            // Fixed-physical-time variational columns satisfy R_q = g V and
            // R_qq = g^2 V_t + (g_q/g) R_q. No variation of the clock is added.
            for (std::size_t i = 24; i < Extra; ++i)
                result[i + 4] = physical[i + 3] * g * g + k * s[i + 7];
        return result;
    }
    template <class F>
    static double quadrature(const F &f, double left, double right, double fl, double fm, double fr,
                             double previous, double tolerance, int depth,
                             std::size_t &evaluations) {
        if (depth == 0 || evaluations > 131072)
            throw PropagationError("Sundman orbital period quadrature did not converge");
        const double middle = (left + right) * .5;
        const double a = f((left + middle) * .5), b = f((middle + right) * .5);
        evaluations += 2;
        const double lo = (middle - left) / 6 * (fl + 4 * a + fm);
        const double hi = (right - middle) / 6 * (fm + 4 * b + fr);
        const double delta = lo + hi - previous;
        if (std::abs(delta) <= 15 * tolerance)
            return lo + hi + delta / 15;
        return quadrature(f, left, middle, fl, a, fm, lo, tolerance * .5, depth - 1, evaluations) +
               quadrature(f, middle, right, fm, b, fr, hi, tolerance * .5, depth - 1, evaluations);
    }
    static double kepler_period(double n, double e) {
        constexpr double pi = std::numbers::pi;
        if (n == 1 || e == 0)
            return 2 * pi;
        if (n == 2)
            return 2 * pi / std::sqrt((1 - e) * (1 + e));
        // Use true anomaly for high n to avoid a narrow periapsis peak in the
        // eccentric-anomaly integrand. Symmetry reduces both integrals to [0,pi].
        const auto f = [=](double angle) {
            const double half = n > 2 ? std::cos(angle * .5) : std::sin(angle * .5);
            return std::pow((1 - e) + 2 * e * half * half, n > 2 ? n - 2 : 1 - n);
        };
        const double fl = f(0), fm = f(pi * .5), fr = f(pi);
        const double whole = pi / 6 * (fl + 4 * fm + fr);
        std::size_t evaluations = 3;
        const double integral =
            quadrature(f, 0, pi, fl, fm, fr, whole, 2e-14 * std::abs(whole), 32, evaluations);
        const double scale = n > 2 ? std::pow((1 - e) * (1 + e), 1.5 - n) : 1;
        return 2 * scale * integral;
    }
};
using SundmanTransform = SundmanSystemTransform<>;
} // namespace orbelle::core::detail
