#pragma once
#include <array>
#include <cmath>
#include <cstdint>
#include <erfa.h>
#include <numbers>
#include <orbelle/core/time/time_provider.hpp>

namespace orbelle::providers::erfa::detail {
using Cip = std::array<double, 3>;

inline Cip direct_cip(core::JulianDate tt) {
    Cip result;
    eraXys06a(tt.day, tt.fraction, &result[0], &result[1], &result[2]);
    return result;
}

// Only the smooth, uncorrected IAU 2006/2000A X,Y,s series is interpolated.
// Earth rotation, observed pole offsets and their rates stay at the exact query
// epoch. Daily degree-eight Chebyshev polynomials avoid evaluating thousands
// of trigonometric terms at every force stage and velocity-difference stencil.
class CipWindow {
    static constexpr int degree = 8;
    static constexpr int sample_degree = 32;
    std::int64_t day_ = 0;
    bool initialized_ = false, usable_ = false;
    Cip origin_{};
    std::array<Cip, degree + 1> coefficients_{};

    [[nodiscard]] Cip polynomial(double x) const {
        Cip result;
        for (std::size_t axis = 0; axis < result.size(); ++axis) {
            double next = 0, following = 0;
            for (int k = degree; k >= 1; --k) {
                const double value = coefficients_[k][axis] + 2 * x * next - following;
                following = next;
                next = value;
            }
            result[axis] = origin_[axis] + coefficients_[0][axis] + x * next - following;
        }
        return result;
    }

  public:
    [[nodiscard]] bool contains(std::int64_t day) const { return initialized_ && day_ == day; }
    void prepare(std::int64_t day) {
        // An integer TT day index gives the same polynomial regardless of query
        // order, propagation direction, leap seconds, or cache eviction.
        day_ = day;
        initialized_ = true;
        usable_ = false;
        coefficients_ = {};
        const auto at = [&](double x) {
            return direct_cip(
                {core::TimeScale::tt, 2451545.0 + static_cast<double>(day), .5 * (x + 1)});
        };
        std::array<Cip, sample_degree + 1> values;
        for (int j = 0; j <= sample_degree; ++j)
            values[j] = at(std::cos(std::numbers::pi * j / sample_degree));
        origin_ = values[sample_degree / 2];
        // Subtract the centre value before the DCT to avoid cancelling the
        // much larger secular offset while resolving the small daily motion.
        for (int k = 0; k <= degree; ++k) {
            // Oversampling keeps rounding noise in the full series from
            // becoming a high-order oscillation of the daily polynomial.
            for (int j = 0; j <= sample_degree; ++j) {
                const double weight = (j == 0 || j == sample_degree ? .5 : 1) *
                                      std::cos(std::numbers::pi * k * j / sample_degree) * 2 /
                                      sample_degree;
                for (std::size_t axis = 0; axis < origin_.size(); ++axis)
                    coefficients_[k][axis] += weight * (values[j][axis] - origin_[axis]);
            }
            if (k == 0)
                for (auto &value : coefficients_[k])
                    value *= .5;
        }
        // Independent, non-node samples detect an unsuitable window. Fall
        // back to the full series instead of publishing an inaccurate cache.
        for (const double x : {-.75, -.25, .25, .75}) {
            const auto exact = at(x), interpolated = polynomial(x);
            for (std::size_t axis = 0; axis < exact.size(); ++axis)
                if (!std::isfinite(interpolated[axis]) ||
                    std::abs(exact[axis] - interpolated[axis]) > 2e-16)
                    return;
        }
        usable_ = true;
    }
    [[nodiscard]] Cip evaluate(double x, core::JulianDate tt) const {
        return usable_ ? polynomial(x) : direct_cip(tt);
    }
};

inline Cip interpolated_cip(core::JulianDate tt) {
    // Preserve the two-part date: adding the small fraction to a Julian day
    // around 2.4 million would discard sub-millisecond time information.
    const double days = tt.day - 2451545.0;
    const double whole_days = std::floor(days);
    const double fraction = (days - whole_days) + tt.fraction;
    const double carry = std::floor(fraction);
    const auto day = static_cast<std::int64_t>(whole_days) + static_cast<std::int64_t>(carry);
    const double x = 2 * (fraction - carry) - 1;
    // A velocity stencil may straddle a day boundary. Bounded thread-local
    // windows share only deterministic model data, never EOP or provider state.
    thread_local std::array<CipWindow, 4> windows;
    thread_local std::size_t next = 0;
    for (const auto &window : windows)
        if (window.contains(day))
            return window.evaluate(x, tt);
    auto &window = windows[next];
    next = (next + 1) % windows.size();
    window.prepare(day);
    return window.evaluate(x, tt);
}
} // namespace orbelle::providers::erfa::detail
