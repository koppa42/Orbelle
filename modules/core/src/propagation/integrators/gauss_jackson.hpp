#pragma once
#include <algorithm>
#include <array>
#include <cmath>
#include <limits>
#include <orbelle/core/propagation/state_provider.hpp>
#include <type_traits>

namespace orbelle::core::detail {
#include "gauss_jackson_coefficients.inc"
// N second-order coordinates plus K first-order states. The latter use the
// method's single-integral sums/coefficients, with no artificial second derivative.
// State layout: coordinates[N], derivatives[N], first-order states[K].
// Thirteen acceleration/derivative samples; compensated sums avoid division by h.
template <std::size_t N, std::size_t K = 0> struct GaussJacksonSystem12 {
    using State = std::array<double, 2 * N + K>;
    using Acceleration = std::array<double, N + K>;
    State state{};
    std::array<Acceleration, 13> history{};
    std::size_t count = 0;
    Acceleration sum1{}, sum2{}, compensation1{}, compensation2{};

    void reset() { count = 0; }
    void recenter_first_order(std::size_t index) {
        if (count == history.size()) {
            Acceleration shift{};
            shift[index - N] = -state[index];
            add(sum1, compensation1, shift);
        }
        state[index] = 0;
    }
    static void add(Acceleration &value, Acceleration &compensation,
                    const Acceleration &increment) {
        for (std::size_t i = 0; i < N + K; ++i) {
            const double change = increment[i] - compensation[i];
            const double next = value[i] + change;
            compensation[i] = (next - value[i]) - change;
            value[i] = next;
        }
    }
    static Acceleration dot(const std::array<double, 13> &weights,
                            const std::array<Acceleration, 13> &values, double total) {
        Acceleration result{}, compensation{};
        for (std::size_t i = 0; i < N + K; ++i)
            result[i] = total * values[0][i];
        // The exact coefficient sum is known. Subtract the newest acceleration to
        // avoid cancellation of a large, nearly constant central acceleration.
        for (std::size_t j = 1; j < values.size(); ++j) {
            Acceleration term{};
            for (std::size_t i = 0; i < N + K; ++i)
                term[i] = weights[j] * (values[j][i] - values[0][i]);
            add(result, compensation, term);
        }
        return result;
    }
    void seed(const State &next, const Acceleration &acceleration, double h,
              const State &state_compensation = {}) {
        for (std::size_t j = std::min(count, history.size() - 1); j > 0; --j)
            history[j] = history[j - 1];
        history[0] = acceleration;
        state = next;
        count = std::min(count + 1, history.size());
        if (count == history.size()) {
            const auto p = dot(gj_position, history, 1.0 / 12);
            const auto v = dot(gj_velocity, history, 0);
            compensation1 = compensation2 = {};
            // Initialize the sums as compensated differences, including the
            // starter's residual and the product rounding. Dropping these bits
            // shifts the physical phase of a long, highly eccentric orbit.
            const auto subtract = [](double value, double residual, double product,
                                     double product_error, double &sum, double &compensation) {
                sum = value - product;
                const double virtual_b = sum - value;
                const double addition_error = (value - (sum - virtual_b)) + (-product - virtual_b);
                compensation = residual + product_error - addition_error;
            };
            for (std::size_t i = 0; i < N + K; ++i) {
                const double velocity_term = h * v[i];
                subtract(state[i + N], state_compensation[i + N], velocity_term,
                         std::fma(h, v[i], -velocity_term), sum1[i], compensation1[i]);
                if (i < N) {
                    const double square = h * h, position_term = square * p[i];
                    const double product_error =
                        std::fma(square, p[i], -position_term) + std::fma(h, h, -square) * p[i];
                    subtract(state[i], state_compensation[i], position_term, product_error, sum2[i],
                             compensation2[i]);
                }
            }
        }
    }
    template <class Force, class Correction, class Converged>
    void advance(double h, bool full_correction, const Force &force, const Correction &correction,
                 const Converged &converged) {
        Acceleration increment{};
        for (std::size_t i = 0; i < N; ++i)
            increment[i] = h * (sum1[i] - compensation1[i]) + h * h * .5 * history[0][i];
        auto next_sum2 = sum2, next_compensation2 = compensation2;
        add(next_sum2, next_compensation2, increment);
        const auto p = dot(gj_position_predict, history, 1.0 / 12);
        const auto v = dot(gj_velocity_predict, history, .5);
        State estimate{};
        for (std::size_t i = 0; i < N + K; ++i) {
            if (i < N)
                estimate[i] = next_sum2[i] + (h * h * p[i] - next_compensation2[i]);
            estimate[i + N] = sum1[i] + (h * (.5 * history[0][i] + v[i]) - compensation1[i]);
        }
        auto shifted = history;
        for (std::size_t j = shifted.size() - 1; j > 0; --j)
            shifted[j] = history[j - 1];
        for (int iteration = 0; iteration < 12; ++iteration) {
            shifted[0] = force(estimate);
            auto next_sum1 = sum1, next_compensation1 = compensation1;
            for (std::size_t i = 0; i < N + K; ++i)
                increment[i] = h * .5 * (history[0][i] + shifted[0][i]);
            add(next_sum1, next_compensation1, increment);
            const auto corrected_p = dot(gj_position, shifted, 1.0 / 12);
            const auto corrected_v = dot(gj_velocity, shifted, 0);
            State corrected{};
            for (std::size_t i = 0; i < N + K; ++i) {
                if (i < N)
                    corrected[i] = next_sum2[i] + (h * h * corrected_p[i] - next_compensation2[i]);
                corrected[i + N] = next_sum1[i] + (h * corrected_v[i] - next_compensation1[i]);
            }
            if (converged(estimate, corrected)) {
                // STK's choice affects the final, post-convergence force update.
                // Pseudo retains perturbations from the last EC evaluation.
                if (full_correction) {
                    shifted[0] = force(corrected);
                } else {
                    shifted[0] = correction(estimate, corrected, shifted[0]);
                }
                for (std::size_t i = 0; i < N + K; ++i)
                    increment[i] = h * .5 * (history[0][i] + shifted[0][i]);
                add(sum1, compensation1, increment);
                sum2 = next_sum2;
                compensation2 = next_compensation2;
                history = shifted;
                state = corrected;
                return;
            }
            estimate = corrected;
        }
        throw PropagationError(
            "HPOP Gauss-Jackson corrector did not converge; reduce the fixed step");
    }
};
// The 6x8 variational block is 24 position/velocity pairs, not 48
// unrelated first-order equations. Applying the single-integral Adams formula
// to both halves introduces its much narrower parasitic stability limit.
// Pack the pairs into the same double-integral recurrence as the orbit.
// An optional Sundman clock remains first-order. Its variational velocities
// must already be dr_variation/dq in the usual velocity scaling.
template <std::size_t Clock> struct VariationalGaussJackson12 : GaussJacksonSystem12<27, Clock> {
    static_assert(Clock <= 1);
    using Base = GaussJacksonSystem12<27, Clock>;
    using State = std::array<double, 54 + Clock>;
    using Acceleration = std::array<double, 51 + Clock>;
    static constexpr std::size_t offset = 6 + Clock;
    static constexpr double velocity_to_position = 1e-3; // 1e3 / 1e6 SI scaling.
    // Public state retains the caller's layout; Base::state uses paired layout.
    State state{};

    static typename Base::State pack(const State &value) {
        typename Base::State result{};
        for (std::size_t i = 0; i < 3; ++i) {
            result[i] = value[i];
            result[27 + i] = value[3 + i];
        }
        for (std::size_t i = 0; i < 24; ++i) {
            result[3 + i] = value[offset + i];
            result[30 + i] = value[offset + 24 + i] * velocity_to_position;
        }
        if constexpr (Clock)
            result[54] = value[6];
        return result;
    }
    static State unpack(const typename Base::State &value) {
        State result{};
        for (std::size_t i = 0; i < 3; ++i) {
            result[i] = value[i];
            result[3 + i] = value[27 + i];
        }
        for (std::size_t i = 0; i < 24; ++i) {
            result[offset + i] = value[3 + i];
            result[offset + 24 + i] = value[30 + i] / velocity_to_position;
        }
        if constexpr (Clock)
            result[6] = value[54];
        return result;
    }
    static typename Base::Acceleration pack_acceleration(const Acceleration &value) {
        typename Base::Acceleration result{};
        std::copy_n(value.begin(), 3, result.begin());
        for (std::size_t i = 0; i < 24; ++i)
            result[3 + i] = value[3 + Clock + 24 + i] * velocity_to_position;
        if constexpr (Clock)
            result[27] = value[3];
        return result;
    }
    static Acceleration unpack_acceleration(const typename Base::Acceleration &value,
                                            const State &reference) {
        Acceleration result{};
        std::copy_n(value.begin(), 3, result.begin());
        for (std::size_t i = 0; i < 24; ++i) {
            result[3 + Clock + i] = reference[offset + 24 + i] * velocity_to_position;
            result[3 + Clock + 24 + i] = value[3 + i] / velocity_to_position;
        }
        if constexpr (Clock)
            result[3] = value[27];
        return result;
    }
    void seed(const State &next, const Acceleration &acceleration, double h,
              const State &state_compensation = {}) {
        const auto packed = pack(next);
        auto residual = pack(state_compensation);
        for (std::size_t i = 0; i < 24; ++i)
            residual[30 + i] -=
                std::fma(next[offset + 24 + i], velocity_to_position, -packed[30 + i]);
        Base::seed(packed, pack_acceleration(acceleration), h, residual);
        state = next;
    }
    void recenter_first_order(std::size_t index) {
        static_assert(Clock == 1);
        (void)index; // The sole first-order state is the clock at public index 6.
        Base::recenter_first_order(54);
        state[6] = 0;
    }
    template <class Force, class Correction, class Converged>
    void advance(double h, bool full_correction, const Force &force, const Correction &correction,
                 const Converged &converged) {
        Base::advance(
            h, full_correction,
            [&](const auto &value) { return pack_acceleration(force(unpack(value))); },
            [&](const auto &before, const auto &after, const auto &acceleration) {
                const auto previous = unpack(before);
                return pack_acceleration(correction(previous, unpack(after),
                                                    unpack_acceleration(acceleration, previous)));
            },
            [&](const auto &before, const auto &after) {
                return converged(unpack(before), unpack(after));
            });
        state = unpack(Base::state);
    }
};
// Optional row-major, scaled 6x8 variational block: STM then D/K sensitivities.
// A column is one vector solution of the coupled variational equations. Its
// components can cross zero without the vector becoming small. Testing each
// component relatively would chase cancellation noise and depend on axis choice.
// Keep each parameter column's own scale: a large STM must not hide a small D/K.
template <std::size_t Extra, class State>
bool variational_corrector_converged(const State &before, const State &after, std::size_t offset,
                                     double epsilon) {
    static_assert(Extra == 0 || Extra == 48);
    if constexpr (Extra > 0) {
        for (std::size_t column = 0; column < 8; ++column) {
            double scale = 1e-6, change = 0;
            for (std::size_t row = 0; row < 6; ++row) {
                const auto i = offset + 8 * row + column;
                if (!std::isfinite(before[i]) || !std::isfinite(after[i]))
                    return false;
                scale = std::max(scale, std::abs(after[i]));
                change = std::max(change, std::abs(after[i] - before[i]));
            }
            if (change > epsilon * scale)
                return false;
        }
    }
    return true;
}
// Ordinary Cartesian-time adapter retains the established convergence and
// pseudo-correction semantics. Other formulations supply their own scales and
// transformed two-body term, including their kinematic clock terms.
template <std::size_t Extra = 0>
struct CartesianGaussJackson12
    : std::conditional_t<Extra == 0, GaussJacksonSystem12<3>, VariationalGaussJackson12<0>> {
    static_assert(Extra == 0 || Extra == 48);
    using Base =
        std::conditional_t<Extra == 0, GaussJacksonSystem12<3>, VariationalGaussJackson12<0>>;
    using State = typename Base::State;
    using Acceleration = typename Base::Acceleration;
    static Acceleration central(const State &s, double gravitational_parameter) {
        const double r = std::hypot(s[0], s[1], s[2]);
        const double scale = -gravitational_parameter / (r * r * r);
        return {scale * s[0], scale * s[1], scale * s[2]};
    }
    template <class Force>
    void advance(double h, double gravitational_parameter, bool full_correction,
                 const Force &force) {
        const auto converged = [](const State &estimate, const State &corrected) {
            double position_change = 0, velocity_change = 0;
            for (std::size_t i = 0; i < 3; ++i) {
                position_change = std::max(position_change, std::abs(corrected[i] - estimate[i]));
                velocity_change =
                    std::max(velocity_change, std::abs(corrected[i + 3] - estimate[i + 3]));
            }
            constexpr double convergence = 64 * std::numeric_limits<double>::epsilon();
            const double position_scale =
                std::max(1., std::hypot(corrected[0], corrected[1], corrected[2]));
            const double velocity_scale =
                std::max(1., std::hypot(corrected[3], corrected[4], corrected[5]));
            if (!variational_corrector_converged<Extra>(estimate, corrected, 6, convergence))
                return false;
            return position_change <= convergence * position_scale &&
                   velocity_change <= convergence * velocity_scale;
        };
        const auto correction = [&, gravitational_parameter](
                                    const State &before, const State &after, Acceleration value) {
            const auto old_central = central(before, gravitational_parameter),
                       new_central = central(after, gravitational_parameter);
            for (std::size_t i = 0; i < 3; ++i)
                value[i] += new_central[i] - old_central[i];
            if constexpr (Extra > 0) {
                const auto updated = force(after);
                std::copy(updated.begin() + 3, updated.end(), value.begin() + 3);
            }
            return value;
        };
        Base::advance(h, full_correction, force, correction, converged);
    }
};
using GaussJackson12 = CartesianGaussJackson12<>;
} // namespace orbelle::core::detail
