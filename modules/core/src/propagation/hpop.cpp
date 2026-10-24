#include "atmosphere_models.hpp"
#include "hpop_force.hpp"
#include "hpop_sampling.hpp"
#include "integrators/gauss_jackson.hpp"
#include "integrators/one_step.hpp"
#include "integrators/shadow_boundary.hpp"
#include "integrators/sundman_propagation.hpp"
#include "integrators/vop.hpp"
#include "srp_geometry.hpp"
#include <algorithm>
#include <bit>
#include <chrono>
#include <cmath>
#include <limits>
#include <mutex>
#include <numeric>
#include <orbelle/core/propagation/hpop.hpp>
#include <orbelle/core/propagation/hpop_variational.hpp>
#include <tuple>
#include <vector>

namespace orbelle::core {
namespace {
std::function<double(Epoch)> sun_distance(const IThirdBodyEphemeris *ephemeris) {
    if (!ephemeris)
        return {};
    return [ephemeris](Epoch at) {
        const auto position = ephemeris->position(ThirdBody::sun, at);
        return std::hypot(position[0], position[1], position[2]);
    };
}
constexpr double checkpoint_seconds = 300;
constexpr std::size_t max_attempts = 200000;
void positive(double v, const char *message) {
    if (!std::isfinite(v) || v <= 0)
        throw std::invalid_argument(message);
}
template <bool Variational = false> class HpopSystem final : public IStateProvider {
    static constexpr std::size_t extra = Variational ? 48 : 0, dimension = 6 + extra;
    using State = std::array<double, dimension>;
    using AcceptedStep = std::function<void(double, const State &, bool)>;
    using RegularizedAcceptedStep =
        std::function<void(double, const State &, bool, std::optional<Epoch>)>;
    using History = detail::CartesianGaussJackson12<extra>;
    using Regularization = detail::SundmanSystemPropagation<extra>;
    static constexpr double scale(std::size_t i) { return i < 3 ? 1e6 : (i < 6 ? 1e3 : 1); }
    HpopVariationalOptions variational_;
    HpopOptions differential_options_;
    std::function<void()> variational_checkpoint_;
    bool initial_drag_above_cutoff_ = false;

    CartesianState initial_;
    HpopOptions options_;
    HpopEnvironment environment_;
    std::optional<Epoch> ephemeris_end_;
    struct CoastDerivative {
        Epoch epoch;
        std::array<std::uint64_t, 6> state;
        std::optional<Epoch> weather;
        std::optional<std::uint64_t> lighting;
        State value;
    };
    // Only a private, end-bounded coast uses this scratch. Public random-access
    // HpopSystem instances may integrate partial queries concurrently and never
    // read or write it. Variational propagation retains its existing checkpoints.
    mutable std::optional<CoastDerivative> coast_derivative_;
    std::optional<Regularization> regularization_;
    // Caches only full, epoch-anchored integration blocks. Partial queries cannot
    // alter the solution used by later queries. Both directions start at t=0.
    mutable std::mutex mutex_;
    mutable std::vector<State> forward_, backward_;
    mutable std::vector<History> gj_forward_, gj_backward_;
    void validate_duration(Epoch epoch) const {
        const auto first = initial_.epoch.tai_nanoseconds(), last = epoch.tai_nanoseconds();
        // Unsigned subtraction gives the exact distance, including opposite Epoch limits.
        const auto duration =
            first < last ? static_cast<std::uint64_t>(last) - static_cast<std::uint64_t>(first)
                         : static_cast<std::uint64_t>(first) - static_cast<std::uint64_t>(last);
        constexpr auto maximum =
            static_cast<std::uint64_t>(hpop_max_duration_seconds) * 1'000'000'000ULL;
        if (duration > maximum)
            throw PropagationError("HPOP query exceeds 366 days from the initial epoch");
    }
    void validate_interval(Epoch epoch) const {
        validate_duration(epoch);
        const double t = epoch.seconds_since(initial_.epoch);
        environment_.validate_weather_interval(
            options_.drag,
            Epoch{std::min(epoch.tai_nanoseconds(), initial_.epoch.tai_nanoseconds())},
            Epoch{std::max(epoch.tai_nanoseconds(), initial_.epoch.tai_nanoseconds())});
        environment_.validate_ephemeris_interval(options_, t < 0 ? epoch : initial_.epoch,
                                                 t < 0 ? initial_.epoch : epoch);
        if (options_.uses_eop_force() && !eop_covers(environment_.eop->data_info(), epoch))
            throw EopCoverageError(
                "HPOP integration requires EOP covering the initial and query epochs");
        if (options_.uses_utc_force())
            (void)environment_.time->to_utc(epoch);
        if constexpr (Variational) {
            environment_.validate_ephemeris_interval(differential_options_,
                                                     t < 0 ? epoch : initial_.epoch,
                                                     t < 0 ? initial_.epoch : epoch);
            if (differential_options_.uses_eop_force() &&
                !eop_covers(environment_.eop->data_info(), epoch))
                throw EopCoverageError("HPOP variational gravity requires EOP coverage");
            if (differential_options_.uses_utc_force())
                (void)environment_.time->to_utc(epoch);
        }
    }
    Epoch epoch_at(double seconds) const {
        const auto start = initial_.epoch.tai_nanoseconds();
        if (ephemeris_end_) {
            const double finish = ephemeris_end_->seconds_since(initial_.epoch);
            if (seconds < std::min(0., finish) || seconds > std::max(0., finish))
                throw PropagationError("HPOP stage exceeds the requested coast");
            if (seconds == finish)
                return *ephemeris_end_;
        }
        auto offset = static_cast<std::int64_t>(std::llround(seconds * 1e9));
        if (ephemeris_end_) {
            // A long relative double interval can round past an integer-ns endpoint.
            // Preserve the exact coast coverage after rounding; this cannot extend it.
            const auto end_offset = ephemeris_end_->tai_nanoseconds() - start;
            offset = std::clamp(offset, std::min(std::int64_t{0}, end_offset),
                                std::max(std::int64_t{0}, end_offset));
        }
        if ((offset > 0 && start > std::numeric_limits<std::int64_t>::max() - offset) ||
            (offset < 0 && start < std::numeric_limits<std::int64_t>::min() - offset))
            throw PropagationError("HPOP stage epoch exceeds the representable time range");
        return Epoch{start + offset};
    }
    // File geomagnetism can jump at UTC boundaries. Stop at every update,
    // including the separately lagged JR and low-altitude MSIS windows.
    std::optional<double> weather_boundary(double t, double end) const {
        const auto &d = options_.drag;
        if (!d.enabled || d.weather_model.empty())
            return {};
        const bool forward = end > t;
        const auto at = epoch_at(t);
        const auto until = epoch_at(end);
        const int period = d.geomagnetic_update == GeomagneticUpdate::daily ? 24 : 3;
        const auto boundary = [&](AtmosphereModel model) -> std::optional<double> {
            const std::int64_t lag =
                model == AtmosphereModel::jacchia_roberts ? 24120000000000LL : 0;
            if (std::min(at.tai_nanoseconds(), until.tai_nanoseconds()) <
                std::numeric_limits<std::int64_t>::min() + lag)
                throw PropagationError("Weather lag exceeds the Epoch range");
            const auto utc = environment_.time->to_utc(Epoch{at.tai_nanoseconds() - lag});
            const auto limit = environment_.time->to_utc(Epoch{until.tai_nanoseconds() - lag});
            const auto fields = [](const UtcDateTime &u) {
                return std::tuple{u.year, u.month, u.day, u.hour, u.minute, u.second, u.nanosecond};
            };
            const auto day = std::chrono::sys_days{
                std::chrono::year{utc.year} / std::chrono::month{static_cast<unsigned>(utc.month)} /
                std::chrono::day{static_cast<unsigned>(utc.day)}};
            int hour = utc.hour / period * period + (forward ? period : 0);
            const auto instant = [&](int h) -> std::optional<double> {
                const int delta = h >= 0 ? h / 24 : (h - 23) / 24;
                const std::chrono::year_month_day date{day + std::chrono::days{delta}};
                const UtcDateTime boundary_utc{
                    static_cast<int>(date.year()),
                    static_cast<int>(static_cast<unsigned>(date.month())),
                    static_cast<int>(static_cast<unsigned>(date.day())),
                    h - delta * 24,
                    0,
                    0,
                    0};
                // The next nominal boundary may lie outside the time provider's
                // coverage; never query it when the requested integration ends earlier.
                if (forward ? fields(boundary_utc) > fields(limit)
                            : fields(boundary_utc) < fields(limit))
                    return {};
                const auto e = environment_.time->from_utc(boundary_utc);
                if (e.tai_nanoseconds() > std::numeric_limits<std::int64_t>::max() - lag)
                    throw PropagationError("Weather boundary exceeds the Epoch range");
                return Epoch{e.tai_nanoseconds() + lag}.seconds_since(initial_.epoch);
            };
            auto next = instant(hour);
            // Compare in the integrator's relative time, so rounding to nanoseconds
            // at an accepted boundary cannot create a zero-length next step.
            if (next && (forward ? *next <= t : *next >= t))
                next = instant(hour + (forward ? period : -period));
            return next;
        };
        auto next = boundary(d.atmosphere);
        if (d.atmosphere == AtmosphereModel::jacchia_roberts &&
            d.low_altitude_model == LowAltitudeDensityModel::nrlmsise00) {
            const auto low = boundary(AtmosphereModel::nrlmsise00);
            if (low)
                next = next ? (forward ? std::min(*next, *low) : std::max(*next, *low)) : low;
        }
        return next;
    }
    std::optional<Epoch> weather_stage(double t, double start, double end) const {
        if (!options_.drag.enabled || options_.drag.weather_model.empty())
            return {};
        const auto first_ns = epoch_at(start).tai_nanoseconds(),
                   last_ns = epoch_at(end).tai_nanoseconds();
        const auto lower = std::min(first_ns, last_ns), upper = std::max(first_ns, last_ns);
        if (lower == upper)
            return Epoch{lower};
        // A step occupies [lower, upper). Endpoint RK stages must use its one-sided
        // weather limit in both directions, rather than averaging across a jump.
        return Epoch{std::clamp(epoch_at(t).tai_nanoseconds(), lower, upper - 1)};
    }
    void validate_state(const State &s) const {
        if constexpr (Variational)
            if (!std::ranges::all_of(s, [](double v) { return std::isfinite(v); }))
                throw PropagationError("HPOP variational state is nonfinite");
        detail::validate_hpop_force_state({s[0], s[1], s[2]}, {s[3], s[4], s[5]}, options_,
                                          environment_);
    }
    bool drag_above_cutoff(const CartesianState &state) const {
        const auto rotation = environment_.frames->gcrf_to_itrf(
            state.epoch, environment_.eop->at(state.epoch).sample);
        const auto position = rotation.apply_position(state.position_m);
        const double height = detail::drag_density_altitude(
            position, drag_geodetic_position(position), options_.drag.use_approximate_altitude);
        return height > detail::drag_cutoff_altitude_m;
    }
    detail::SrpGeometry srp_geometry(double t, const State &state) const {
        return detail::resolve_srp_geometry(epoch_at(t), {state[0], state[1], state[2]},
                                            {state[3], state[4], state[5]}, options_.srp,
                                            *environment_.ephemeris);
    }
    bool mitigates_shadow() const {
        const auto &o = options_.srp;
        return o.boundary_mitigation && o.enabled && o.shadow_model != SrpShadowModel::none &&
               (Variational || (o.cr != 0 && o.area_mass_ratio_m2_per_kg != 0)) &&
               (o.earth_shadow || o.moon_shadow || !o.additional_eclipsing_bodies.empty());
    }
    detail::ShadowContacts shadow_contacts(double t, const State &state) const {
        const auto geometry = srp_geometry(t, state);
        return detail::srp_shadow_contacts({state[0], state[1], state[2]}, geometry.sun,
                                           options_.srp, geometry.moon,
                                           std::span{geometry.additional}.first(geometry.count));
    }
    std::optional<double> cylinder_lighting(double begin, const State &state, double end) const {
        if (!mitigates_shadow() || options_.srp.shadow_model != SrpShadowModel::cylindrical)
            return {};
        // Choose the outgoing regime at a located contact. Keep this infinitesimal
        // probe inside the requested interval; it never changes the propagated state.
        const double dt = std::copysign(
            std::min(std::abs(end - begin), 4 * detail::shadow_time_resolution(begin, end)),
            end - begin);
        State ahead = state;
        for (std::size_t i = 0; i < 3; ++i)
            ahead[i] = std::fma(dt, state[i + 3], state[i]);
        const auto geometry = srp_geometry(begin + dt, ahead);
        return evaluate_srp({ahead[0], ahead[1], ahead[2]}, geometry.sun, options_.srp,
                            geometry.moon, std::span{geometry.additional}.first(geometry.count))
            .lighting_ratio;
    }
    template <class Probe>
    std::optional<double>
    shadow_boundary(double begin, const State &initial, double end, const State &final,
                    const Probe &probe, std::size_t &attempts,
                    const std::function<void()> &checkpoint, bool probe_counts_work = false) const {
        if (!mitigates_shadow())
            return {};
        return detail::first_shadow_boundary(begin, end, [&](double at) {
            if (checkpoint)
                checkpoint();
            State value;
            if (at == end)
                value = final;
            else if (std::abs(at - begin) <= 8 * detail::shadow_time_resolution(begin, end)) {
                // Only the outgoing-side epsilon sample uses a local Taylor
                // position. Its O(dt^2) error is below double position resolution;
                // avoid solving a nanosecond Sundman coast just to choose a side.
                value = initial;
                for (std::size_t i = 0; i < 3; ++i)
                    value[i] = std::fma(at - begin, initial[i + 3], initial[i]);
            } else {
                if (!probe_counts_work && ++attempts > max_attempts)
                    throw PropagationError(
                        "HPOP exceeded 200000 integration attempts in one query");
                value = probe(at);
            }
            validate_state(value);
            return shadow_contacts(at, value);
        });
    }
    bool correct_penumbra(double time, const State &value) const {
        if (!mitigates_shadow() || options_.srp.shadow_model != SrpShadowModel::dual_cone)
            return false;
        const auto geometry = srp_geometry(time, value);
        const auto ratio =
            evaluate_srp({value[0], value[1], value[2]}, geometry.sun, options_.srp, geometry.moon,
                         std::span{geometry.additional}.first(geometry.count))
                .lighting_ratio;
        if (ratio > 0 && ratio < 1)
            return true;
        const auto contacts = shadow_contacts(time, value);
        for (std::size_t i = 0; i < contacts.size; ++i)
            if (std::abs(contacts.values[i]) < 2e-9)
                return true;
        return false;
    }
    State derivative(double t, const State &s, std::optional<Epoch> weather_at = {},
                     std::optional<double> shadow_lighting = {}) const {
        validate_state(s);
        const CartesianState state{
            epoch_at(t), OriginId::earth, FrameId::gcrf, {s[0], s[1], s[2]}, {s[3], s[4], s[5]}};
        if constexpr (Variational) {
            // Reuse the force's density height to guard hard-cutoff crossings;
            // another frame transform at every stage would duplicate expensive work.
            const auto force = detail::hpop_differential_unchecked(
                state, options_, environment_, weather_at,
                (differential_options_.gravity_degree != options_.gravity_degree ||
                 differential_options_.gravity_order != options_.gravity_order)
                    ? &differential_options_
                    : nullptr,
                variational_checkpoint_, initial_drag_above_cutoff_);
            const auto &a = force.force.acceleration_mps2;
            State result{s[3], s[4], s[5], a[0], a[1], a[2]};
            for (std::size_t i = 0; i < 3; ++i)
                for (std::size_t col = 0; col < 8; ++col) {
                    result[6 + 8 * i + col] = s[6 + 8 * (i + 3) + col] * scale(i + 3) / scale(i);
                    double rate = 0;
                    for (std::size_t j = 0; j < 3; ++j) {
                        rate = std::fma(force.force.position_jacobian_per_s2[i][j] * scale(j) /
                                            scale(i + 3),
                                        s[6 + 8 * j + col], rate);
                        rate = std::fma(force.force.velocity_jacobian_per_s[i][j],
                                        s[6 + 8 * (j + 3) + col], rate);
                    }
                    if (col == 6)
                        rate += force.drag_parameter_jacobian_pa[i] / scale(i + 3);
                    if (col == 7)
                        rate += force.srp_parameter_jacobian_pa[i] / scale(i + 3);
                    result[6 + 8 * (i + 3) + col] = rate;
                }
            return result;
        } else {
            const auto evaluate = [&] {
                const auto acceleration = detail::hpop_acceleration_unchecked(
                    state, options_, environment_, weather_at, shadow_lighting);
                return State{s[3], s[4], s[5], acceleration[0], acceleration[1], acceleration[2]};
            };
            if (!ephemeris_end_)
                return evaluate();
            const auto key = std::bit_cast<std::array<std::uint64_t, 6>>(s);
            const auto lighting =
                shadow_lighting ? std::optional{std::bit_cast<std::uint64_t>(*shadow_lighting)}
                                : std::nullopt;
            if (coast_derivative_ && coast_derivative_->epoch == state.epoch &&
                coast_derivative_->state == key && coast_derivative_->weather == weather_at &&
                coast_derivative_->lighting == lighting)
                return coast_derivative_->value;
            const auto result = evaluate();
            // Endpoint validation still runs above. Reuse its successful force
            // evaluation only for exactly the same state and force boundary side.
            coast_derivative_ = CoastDerivative{state.epoch, key, weather_at, lighting, result};
            return result;
        }
    }
    using TrialStep = detail::TrialStep<dimension>;
    detail::ErrorControl<dimension> error_control(bool startup = false) const {
        detail::ErrorControl<dimension> result;
        result.fixed = !startup && options_.uses_fixed_step();
        result.relative =
            startup ? (mitigates_shadow() ? 1e-15 : 1e-14) : options_.relative_tolerance;
        result.max_step = options_.max_step_seconds;
        for (std::size_t i = 0; i < 6; ++i)
            result.absolute[i] =
                startup ? (i < 3 ? (mitigates_shadow() ? 1e-10 : 1e-9)
                                 : (mitigates_shadow() ? 1e-13 : 1e-12))
                        : (i < 3 ? options_.position_tolerance_m : options_.velocity_tolerance_mps);
        for (std::size_t i = 6; i < dimension; ++i)
            result.absolute[i] = startup ? std::min(variational_.absolute_tolerance, 1e-15)
                                         : variational_.absolute_tolerance;
        return result;
    }
    auto step_derivative(double begin, double end, const State &initial,
                         std::optional<double> lighting = {}) const {
        if (!lighting)
            lighting = cylinder_lighting(begin, initial, end);
        return [this, begin, end, lighting](double at, const State &state) {
            return derivative(at, state, weather_stage(at, begin, end), lighting);
        };
    }
    State rk4(double t, const State &s, double h) const {
        return detail::rk4(t, s, h, step_derivative(t, t + h, s));
    }
    TrialStep rkf78(double t, const State &s, double h,
                    const std::function<void()> &checkpoint) const {
        const auto force = step_derivative(t, t + h, s);
        const auto control = error_control();
        if (options_.uses_vop())
            return detail::vop_step(
                t, s, h, options_.gravity_mu, false, force,
                [&](const auto &derivative, const auto &scales) {
                    return detail::rkf78(0, s, h, derivative, scales);
                },
                control, checkpoint);
        return detail::rkf78(t, s, h, force, control);
    }
    TrialStep rkv89(double t, const State &s, double h) const {
        return detail::rkv89(t, s, h, step_derivative(t, t + h, s), error_control());
    }
    TrialStep bulirsch_stoer(double t, const State &s, double h,
                             const std::function<void()> &checkpoint, bool startup = false,
                             std::optional<double> lighting = {}) const {
        const auto force = step_derivative(t, t + h, s, lighting);
        const auto control = error_control(startup);
        if (options_.uses_vop())
            return detail::vop_step(
                t, s, h, options_.gravity_mu, false, force,
                [&](const auto &derivative, const auto &scales) {
                    return detail::bulirsch_stoer(0, s, h, derivative, scales, checkpoint, startup);
                },
                control, checkpoint);
        return detail::bulirsch_stoer(t, s, h, force, control, checkpoint, startup);
    }
    TrialStep trial_step(double t, const State &s, double h,
                         const std::function<void()> &checkpoint) const {
        switch (options_.integrator) {
        case HpopIntegrator::rkf78:
            return rkf78(t, s, h, checkpoint);
        case HpopIntegrator::rkv89:
            return rkv89(t, s, h);
        case HpopIntegrator::bulirsch_stoer:
            return bulirsch_stoer(t, s, h, checkpoint);
        case HpopIntegrator::rk4:
            return {rk4(t, s, h), 0, 0};
        case HpopIntegrator::gauss_jackson:
            break; // Multistep history is handled by gauss_jackson_integrate.
        }
        throw std::invalid_argument("Unsupported HPOP integrator");
    }
    State integrate(State state, double t, double end, std::size_t &attempts,
                    const std::function<void()> &checkpoint, const AcceptedStep &accepted = {},
                    double *continued_step = nullptr) const {
        // A large allowed ceiling is not a suitable first trial in a low orbit:
        // huge RK stages can enter the body before an error estimate is available.
        // Seed from local gravitational and crossing time scales, then let error
        // control grow/shrink the step over the full configured range.
        const double radius = std::hypot(state[0], state[1], state[2]);
        const double speed = std::hypot(state[3], state[4], state[5]);
        const double dynamical = radius / std::sqrt(options_.gravity_mu / radius);
        const double crossing = speed > 0 ? radius / speed : dynamical;
        double step = std::clamp(.1 * std::min(dynamical, crossing), options_.min_step_seconds,
                                 options_.max_step_seconds);
        if (continued_step && *continued_step > 0)
            step =
                std::clamp(*continued_step, options_.min_step_seconds, options_.max_step_seconds);
        while (t != end) {
            if (checkpoint)
                checkpoint();
            if (++attempts > max_attempts)
                throw PropagationError("HPOP exceeded 200000 integration attempts in one query");
            const double remaining = std::abs(end - t);
            double magnitude = std::min(step, remaining);
            const auto boundary = weather_boundary(t, end);
            if (boundary)
                magnitude = std::min(magnitude, std::abs(*boundary - t));
            double h = std::copysign(magnitude, end - t);
            if (t + h == t)
                throw PropagationError("HPOP integration step cannot advance time");
            bool correction = correct_penumbra(t, state);
            const auto evaluate = [&](double at) -> TrialStep {
                if (correction)
                    return {one_sided_startup(state, t, at, attempts, checkpoint), 0, 0};
                return trial_step(t, state, at - t, checkpoint);
            };
            auto trial = evaluate(t + h);
            std::optional<double> shadow;
            if (trial.error <= 1) {
                const double target = magnitude == remaining ? end : t + h;
                shadow = shadow_boundary(
                    t, state, target, trial.value, [&](double at) { return evaluate(at).value; },
                    attempts, checkpoint, correction);
                if (shadow && !correction &&
                    options_.srp.shadow_model == SrpShadowModel::dual_cone) {
                    correction = true;
                    trial = evaluate(target);
                    shadow = shadow_boundary(
                        t, state, target, trial.value,
                        [&](double at) { return evaluate(at).value; }, attempts, checkpoint, true);
                }
                if (shadow) {
                    h = *shadow - t;
                    magnitude = std::abs(h);
                    trial = evaluate(*shadow);
                }
            }
            const auto &next = trial.value;
            const double error = trial.error, exponent = trial.exponent;
            if (!std::isfinite(error) ||
                !std::all_of(next.begin(), next.end(), [](double v) { return std::isfinite(v); }))
                throw PropagationError("HPOP integration produced a nonfinite result");
            if (error <= 1) {
                const bool at_boundary =
                    shadow || (boundary && magnitude == std::abs(*boundary - t));
                state = next;
                t = shadow ? *shadow : (magnitude == remaining ? end : t + h);
                // Validate the accepted endpoint as well as all RK stages.
                (void)derivative(t, state);
                if (accepted)
                    accepted(t, state, at_boundary);
            } else if (magnitude <= options_.min_step_seconds) {
                throw PropagationError("HPOP cannot meet tolerance at the minimum step");
            }
            const double factor =
                error == 0 ? 4 : std::clamp(0.9 * std::pow(error, exponent), 0.1, 4.0);
            step = std::clamp(magnitude * factor, options_.min_step_seconds,
                              options_.max_step_seconds);
        }
        if (continued_step)
            *continued_step = step;
        return state;
    }

    State integrate_fixed(State state, double t, double end, std::size_t first_node,
                          std::size_t &attempts, const std::function<void()> &checkpoint,
                          const AcceptedStep &accepted = {}) const {
        const double step = std::copysign(options_.fixed_step_seconds, end - t);
        std::size_t node = first_node;
        while (t != end) {
            const double nominal = static_cast<double>(++node) * step;
            const double stop = step > 0 ? std::min(nominal, end) : std::max(nominal, end);
            // A known force discontinuity splits the current grid cell; the next cell
            // still ends on the original epoch-anchored grid.
            while (t != stop) {
                if (checkpoint)
                    checkpoint();
                if (++attempts > max_attempts)
                    throw PropagationError(
                        "HPOP exceeded 200000 integration attempts in one query");
                const auto boundary = weather_boundary(t, stop);
                double target = boundary.value_or(stop);
                const double h = target - t;
                if (h == 0 || t + h == t)
                    throw PropagationError("HPOP fixed integration step cannot advance time");
                // The 3/2-power circular-segment onset is not a smooth ODE.
                // Correct penumbra/adjacent contact steps with a high-accuracy
                // one-sided BS bridge. Smooth arcs retain the selected method.
                bool correction = correct_penumbra(t, state);
                const auto evaluate = [&](double at) {
                    return correction ? one_sided_startup(state, t, at, attempts, checkpoint)
                                      : trial_step(t, state, at - t, checkpoint).value;
                };
                auto next = evaluate(target);
                auto shadow = shadow_boundary(t, state, target, next, evaluate, attempts,
                                              checkpoint, correction);
                if (shadow && !correction &&
                    options_.srp.shadow_model == SrpShadowModel::dual_cone) {
                    correction = true;
                    next = evaluate(target);
                    shadow = shadow_boundary(t, state, target, next, evaluate, attempts, checkpoint,
                                             true);
                }
                if (shadow) {
                    target = *shadow;
                    next = evaluate(target);
                }
                state = next;
                (void)derivative(target, state);
                t = target;
                if (accepted)
                    accepted(t, state, shadow || (boundary && t == *boundary));
            }
        }
        return state;
    }
    State fixed_at(double t, std::size_t &attempts, const std::function<void()> &checkpoint) const {
        const auto nodes = static_cast<std::size_t>(
            std::max(1., std::ceil(checkpoint_seconds / options_.fixed_step_seconds)));
        const double interval = nodes * options_.fixed_step_seconds;
        const double sign = t < 0 ? -1 : 1;
        auto index = static_cast<std::size_t>(std::floor(std::abs(t) / interval));
        const auto grid = [&](std::size_t block) {
            return static_cast<double>(block * nodes) * options_.fixed_step_seconds;
        };
        if (index > 0 && grid(index) > std::abs(t))
            --index;
        if (grid(index + 1) <= std::abs(t))
            ++index;
        State value;
        {
            const std::lock_guard lock{mutex_};
            auto &cache = t < 0 ? backward_ : forward_;
            while (cache.size() <= index) {
                const auto block = cache.size() - 1;
                const auto next =
                    integrate_fixed(cache.back(), sign * grid(block), sign * grid(block + 1),
                                    block * nodes, attempts, checkpoint);
                cache.push_back(next);
            }
            value = cache[index];
        }
        return integrate_fixed(value, sign * grid(index), t, index * nodes, attempts, checkpoint);
    }

    State one_sided_startup(State state, double t, double end, std::size_t &attempts,
                            const std::function<void()> &checkpoint,
                            std::optional<double> lighting = {}) const {
        if (!lighting)
            lighting = cylinder_lighting(t, state, end);
        // Accurate one-sided seeding and fractional output. It never obtains history
        // before the initial epoch or after the requested interval.
        double step = std::abs(end - t);
        while (t != end) {
            if (checkpoint)
                checkpoint();
            if (++attempts > max_attempts)
                throw PropagationError("HPOP exceeded 200000 integration attempts in one query");
            const double remaining = std::abs(end - t);
            const double magnitude = std::min(step, remaining);
            const double h = std::copysign(magnitude, end - t);
            if (t + h == t)
                throw PropagationError("HPOP startup step cannot advance time");
            const auto result = bulirsch_stoer(t, state, h, checkpoint, true, lighting);
            validate_state(result.value);
            if (!std::isfinite(result.error))
                throw PropagationError("HPOP startup produced a nonfinite error estimate");
            if (result.error <= 1) {
                state = result.value;
                t = magnitude == remaining ? end : t + h;
            } else if (magnitude <= 1e-9) {
                throw PropagationError(
                    "HPOP cannot initialize Gauss-Jackson at nanosecond resolution");
            }
            const double factor =
                result.error == 0
                    ? 4
                    : std::clamp(.9 * std::pow(result.error, result.exponent), .1, 4.);
            step = std::max(1e-9, magnitude * factor);
        }
        return state;
    }
    History gauss_jackson_integrate(History history, double begin, double end,
                                    std::size_t first_node, std::size_t &attempts,
                                    const std::function<void()> &checkpoint,
                                    const AcceptedStep &accepted = {}) const {
        const double h = std::copysign(options_.fixed_step_seconds, end - begin);
        double t = begin;
        std::size_t node = 0;
        while (t != end) {
            const double nominal = static_cast<double>(first_node + ++node) * h;
            const double until = h > 0 ? std::min(nominal, end) : std::max(nominal, end);
            if (until == t)
                throw PropagationError("HPOP fixed step cannot advance time");
            while (t != until) {
                if (checkpoint)
                    checkpoint();
                const auto weather = weather_boundary(t, until);
                double target = weather.value_or(until);
                const auto lighting = cylinder_lighting(t, history.state, target);
                auto candidate = history;
                const bool bridge = correct_penumbra(t, history.state) || target != nominal ||
                                    t != static_cast<double>(first_node + node - 1) * h;
                if (bridge) {
                    candidate.state =
                        one_sided_startup(history.state, t, target, attempts, checkpoint, lighting);
                    candidate.reset();
                } else {
                    const auto force = [&](double at, const State &value) {
                        if (checkpoint)
                            checkpoint();
                        const auto d =
                            derivative(at, value, weather_stage(at, t, target), lighting);
                        typename History::Acceleration result{};
                        std::copy(d.begin() + 3, d.end(), result.begin());
                        return result;
                    };
                    if (candidate.count == 0)
                        candidate.seed(candidate.state, force(t, candidate.state), h);
                    if (candidate.count < candidate.history.size()) {
                        const auto next = one_sided_startup(candidate.state, t, target, attempts,
                                                            checkpoint, lighting);
                        candidate.seed(next, force(target, next), h);
                    } else {
                        if (++attempts > max_attempts)
                            throw PropagationError(
                                "HPOP exceeded 200000 integration attempts in one query");
                        candidate.advance(h, options_.gravity_mu,
                                          options_.correction == HpopCorrection::full,
                                          [&](const State &value) { return force(target, value); });
                        validate_state(candidate.state);
                    }
                }
                const auto shadow = shadow_boundary(
                    t, history.state, target, candidate.state,
                    [&](double at) {
                        return one_sided_startup(history.state, t, at, attempts, checkpoint,
                                                 lighting);
                    },
                    attempts, checkpoint, true);
                if (shadow) {
                    target = *shadow;
                    candidate.state =
                        one_sided_startup(history.state, t, target, attempts, checkpoint, lighting);
                }
                const bool boundary = shadow || (weather && target == *weather);
                if (boundary)
                    candidate.reset();
                history = std::move(candidate);
                t = target;
                if (accepted)
                    accepted(t, history.state, boundary);
            }
        }
        return history;
    }
    State gauss_jackson_at(double t, std::size_t &attempts,
                           const std::function<void()> &checkpoint) const {
        // Cache boundaries lie on the fixed grid. A checkpoint includes the sums and
        // acceleration history, so 300 s cache boundaries never restart the method.
        const auto nodes = static_cast<std::size_t>(
            std::max(1., std::ceil(checkpoint_seconds / options_.fixed_step_seconds)));
        const double interval = nodes * options_.fixed_step_seconds;
        const double sign = t < 0 ? -1 : 1;
        auto index = static_cast<std::size_t>(std::floor(std::abs(t) / interval));
        const auto grid = [&](std::size_t block) {
            return static_cast<double>(block * nodes) * options_.fixed_step_seconds;
        };
        if (index > 0 && grid(index) > std::abs(t))
            --index;
        if (grid(index + 1) <= std::abs(t))
            ++index;
        History history;
        double at;
        {
            const std::lock_guard lock{mutex_};
            auto &cache = t < 0 ? gj_backward_ : gj_forward_;
            if (cache.empty()) {
                History initial;
                initial.state = forward_.front();
                cache.push_back(initial);
            }
            while (cache.size() <= index) {
                const double begin = sign * grid(cache.size() - 1);
                const double end = sign * grid(cache.size());
                const auto next = gauss_jackson_integrate(
                    cache.back(), begin, end, (cache.size() - 1) * nodes, attempts, checkpoint);
                cache.push_back(next);
            }
            history = cache[index];
            at = sign * grid(index);
        }
        return gauss_jackson_integrate(history, at, t, index * nodes, attempts, checkpoint).state;
    }

    State epoch_output(double time, const State &state, Epoch when,
                       std::optional<Epoch> weather) const {
        // Accepted transformed/shadow nodes need not fall on integer nanoseconds. Transport
        // the output state to its precise Epoch instead of relabeling the state:
        // otherwise micrometre position jitter is amplified by Hermite derivatives.
        // Split whole seconds so even a long coast retains its fractional nanosecond.
        const auto ns = when.tai_nanoseconds() - initial_.epoch.tai_nanoseconds();
        const double shift = std::fma(static_cast<double>(ns % 1'000'000'000), 1e-9,
                                      static_cast<double>(ns / 1'000'000'000) - time);
        if (shift == 0)
            return state;
        const auto rate = derivative(time, state, weather);
        auto output = state;
        for (std::size_t axis = 0; axis < output.size(); ++axis)
            output[axis] = std::fma(shift, rate[axis], state[axis]);
        validate_state(output);
        return output;
    }
    typename Regularization::State
    integrate_regularized(typename Regularization::State state, double end, std::size_t &attempts,
                          const std::function<void()> &checkpoint,
                          const RegularizedAcceptedStep &accepted = {}) const {
        while (state.time() != end) {
            const auto boundary = weather_boundary(state.time(), end);
            const double until = boundary.value_or(end), begin = state.segment_begin;
            const auto lighting = cylinder_lighting(state.time(), state.cartesian(), until);
            const auto force = [&](double time, const State &value) {
                const auto rate =
                    derivative(time, value, weather_stage(time, begin, until), lighting);
                std::array<double, 3 + extra> result{};
                std::copy(rate.begin() + 3, rate.end(), result.begin());
                return result;
            };
            bool shadow = false;
            typename Regularization::StepBoundary detect;
            if (mitigates_shadow())
                detect = [&](const typename Regularization::State &first,
                             const typename Regularization::State &last)
                    -> std::optional<typename Regularization::State> {
                    const auto contact = shadow_boundary(
                        first.time(), first.cartesian(), last.time(), last.cartesian(),
                        [&](double at) {
                            return regularization_
                                ->boundary_prefix(first, at, force, attempts, checkpoint,
                                                  options_.srp.shadow_model ==
                                                      SrpShadowModel::dual_cone)
                                .cartesian();
                        },
                        attempts, checkpoint, true);
                    if (!contact)
                        return {};
                    auto prefix = regularization_->boundary_prefix(
                        first, *contact, force, attempts, checkpoint,
                        options_.srp.shadow_model == SrpShadowModel::dual_cone);
                    shadow = true;
                    return prefix;
                };
            state = regularization_->propagate(
                std::move(state), until, force, attempts, checkpoint,
                [&](double time, const State &value) {
                    validate_state(value);
                    if (accepted)
                        accepted(time, value, boundary && time == until,
                                 weather_stage(time, begin, until));
                },
                detect,
                [&](double time, const State &value) { return correct_penumbra(time, value); });
            if (shadow && accepted)
                accepted(state.time(), state.cartesian(), true,
                         weather_stage(state.time(), begin, until));
            if (shadow || boundary)
                state.restart_force_segment();
        }
        return state;
    }

  public:
    HpopSystem(CartesianState initial, HpopOptions options, HpopEnvironment environment,
               std::optional<Epoch> ephemeris_end = {}, HpopVariationalOptions variational = {},
               std::function<void()> variational_checkpoint = {})
        : variational_(std::move(variational)), differential_options_(options),
          variational_checkpoint_(std::move(variational_checkpoint)), initial_(initial),
          options_(options), environment_(std::move(environment)), ephemeris_end_(ephemeris_end) {
        validate_hpop_options(options_);
        if (ephemeris_end_)
            validate_duration(*ephemeris_end_);
        detail::prepare_hpop_force_environment(initial, options_, environment_);
        if constexpr (Variational) {
            if (options_.drag.enabled)
                initial_drag_above_cutoff_ = drag_above_cutoff(initial_);
            if (!std::isfinite(variational_.absolute_tolerance) ||
                variational_.absolute_tolerance < 1e-16 || variational_.absolute_tolerance > 1)
                throw std::invalid_argument("Variational absolute tolerance must be 1e-16..1");
            if (options_.srp.enabled && options_.srp.shadow_model == SrpShadowModel::cylindrical &&
                (options_.srp.earth_shadow || options_.srp.moon_shadow ||
                 !options_.srp.additional_eclipsing_bodies.empty()))
                throw std::invalid_argument(
                    "Cylindrical shadow variational propagation requires saltation");
            if (options_.drag.enabled &&
                options_.drag.atmosphere == AtmosphereModel::jacchia_roberts &&
                options_.drag.low_altitude_model != LowAltitudeDensityModel::none &&
                options_.drag.blending_range_m == 0)
                throw std::invalid_argument(
                    "Hard atmosphere switching requires variational saltation");
            if (variational_.gravity) {
                differential_options_.gravity_degree = variational_.gravity->degree;
                differential_options_.gravity_order = variational_.gravity->order;
                detail::prepare_hpop_force_environment(initial, differential_options_,
                                                       environment_);
            }
        }
        State value{};
        for (std::size_t i = 0; i < 3; ++i) {
            value[i] = initial.position_m[i];
            value[i + 3] = initial.velocity_mps[i];
        }
        if constexpr (Variational)
            for (std::size_t i = 0; i < 6; ++i)
                value[6 + 8 * i + i] = 1;
        (void)derivative(0, value);
        if (options_.uses_vop())
            (void)detail::universal_kepler_linearized(
                {value[0], value[1], value[2], value[3], value[4], value[5]}, 0,
                options_.gravity_mu);
        if (options_.time_regularization)
            regularization_.emplace(value, options_.gravity_mu, options_.regularization_exponent,
                                    static_cast<std::size_t>(options_.steps_per_orbit), options_,
                                    variational_.absolute_tolerance);
        forward_.push_back(value);
        backward_.push_back(value);
    }
    HpopVariationalState variational_at(Epoch end, const std::function<void()> &checkpoint) const
        requires Variational
    {
        validate_interval(end);
        const double t = end.seconds_since(initial_.epoch);
        std::size_t attempts = 0;
        State value = forward_.front();
        if (end != initial_.epoch) {
            if (regularization_)
                value =
                    integrate_regularized(regularization_->initial_state(), t, attempts, checkpoint)
                        .cartesian();
            else if (options_.integrator == HpopIntegrator::gauss_jackson) {
                History history;
                history.state = value;
                value = gauss_jackson_integrate(history, 0, t, 0, attempts, checkpoint).state;
            } else if (options_.uses_fixed_step())
                value = integrate_fixed(value, 0, t, 0, attempts, checkpoint);
            else
                value = integrate(value, 0, t, attempts, checkpoint);
            value = epoch_output(t, value, end, weather_stage(t, 0, t));
        }
        if (checkpoint)
            checkpoint();
        return variational_output(end, value);
    }
    HpopVariationalState variational_output(Epoch end, const State &value) const
        requires Variational
    {
        HpopVariationalState result{{end,
                                     OriginId::earth,
                                     FrameId::gcrf,
                                     {value[0], value[1], value[2]},
                                     {value[3], value[4], value[5]}}};
        for (std::size_t i = 0; i < 6; ++i) {
            for (std::size_t j = 0; j < 6; ++j)
                result.state_transition[i][j] = value[6 + 8 * i + j] * scale(i) / scale(j);
            result.drag_sensitivity[i] = value[6 + 8 * i + 6] * scale(i);
            result.srp_sensitivity[i] = value[6 + 8 * i + 7] * scale(i);
        }
        return result;
    }
    void
    sample_variational(const std::vector<Epoch> &times, std::span<const std::size_t> indices,
                       const std::function<void(std::size_t, const HpopVariationalState &)> &sample,
                       const std::function<void()> &checkpoint) const
        requires Variational
    {
        State value = forward_.front();
        History history;
        history.state = value;
        auto regular_state =
            regularization_ ? regularization_->initial_state() : typename Regularization::State{};
        double at = 0, last_begin = 0, continued_step = 0;
        std::size_t attempts = 0;
        for (const auto index : indices) {
            if (checkpoint)
                checkpoint();
            const auto epoch = times[index];
            const double until = epoch.seconds_since(initial_.epoch);
            if (until != at) {
                last_begin = at;
                if (regularization_) {
                    regular_state = integrate_regularized(std::move(regular_state), until, attempts,
                                                          checkpoint);
                    value = regular_state.cartesian();
                } else if (options_.uses_fixed_step()) {
                    auto node = static_cast<std::size_t>(
                        std::floor(std::abs(at) / options_.fixed_step_seconds));
                    const auto grid = [&](std::size_t n) {
                        return n * options_.fixed_step_seconds;
                    };
                    if (node && grid(node) > std::abs(at))
                        --node;
                    if (grid(node + 1) <= std::abs(at))
                        ++node;
                    if (options_.integrator == HpopIntegrator::gauss_jackson) {
                        history =
                            gauss_jackson_integrate(history, at, until, node, attempts, checkpoint);
                        value = history.state;
                    } else
                        value = integrate_fixed(value, at, until, node, attempts, checkpoint);
                } else
                    value = integrate(value, at, until, attempts, checkpoint, {}, &continued_step);
                at = until;
            }
            // Correct only the emitted copy for integer-ns rounding. Feeding this
            // correction into the continued state would accumulate output-grid noise.
            const auto output = epoch_output(at, value, epoch, weather_stage(at, last_begin, at));
            sample(index, variational_output(epoch, output));
        }
        if (checkpoint)
            checkpoint();
    }
    void validate_range(Epoch first, Epoch last) const {
        validate_interval(first);
        validate_interval(last);
    }
    std::vector<OrbitEphemerisArc> ephemeris_once(Epoch end, std::int64_t reporting_step,
                                                  const std::function<void()> &checkpoint,
                                                  std::size_t minimum_nodes,
                                                  const std::vector<std::int64_t> &extra_gates,
                                                  std::size_t &attempts) const {
        if (checkpoint)
            checkpoint();
        if (reporting_step < 0 || reporting_step > 86400'000'000'000LL)
            throw std::invalid_argument("HPOP reporting step must be 0..86400 seconds");
        validate_interval(end);
        if (end == initial_.epoch)
            return {{initial_}};
        // The duration check above makes this integer subtraction safe even near Epoch limits.
        const auto offset = end.tai_nanoseconds() - initial_.epoch.tai_nanoseconds();
        const std::int64_t sign = offset < 0 ? -1 : 1, duration = offset * sign;
        const double finish = end.seconds_since(initial_.epoch);
        std::vector<OrbitEphemerisArc> arcs{{initial_}};
        State value{};
        for (std::size_t i = 0; i < 3; ++i) {
            value[i] = initial_.position_m[i];
            value[i + 3] = initial_.velocity_mps[i];
        }
        History history;
        history.state = value;
        auto regular_state =
            regularization_ ? regularization_->initial_state() : typename Regularization::State{};
        double at = 0, continued_step = 0;
        std::vector<std::int64_t> support_gates;
        if (minimum_nodes > 1 && !reporting_step) {
            // Short coasts/weather arcs still need a complete polynomial stencil. Add
            // real integration endpoints inside that arc, without extending coverage or
            // lowering the selected order. Longer arcs retain the configured step grid.
            const auto intervals = static_cast<std::int64_t>(minimum_nodes - 1);
            const double ceiling = options_.uses_fixed_step() ? options_.fixed_step_seconds
                                                              : options_.max_step_seconds;
            std::int64_t begin = 0;
            while (begin < duration) {
                if (checkpoint)
                    checkpoint();
                const Epoch begin_epoch{initial_.epoch.tai_nanoseconds() + sign * begin};
                const auto boundary =
                    weather_boundary(begin_epoch.seconds_since(initial_.epoch), finish);
                const auto stop = boundary ? sign * (epoch_at(*boundary).tai_nanoseconds() -
                                                     initial_.epoch.tai_nanoseconds())
                                           : duration;
                if (stop <= begin || stop > duration)
                    throw PropagationError("HPOP support interval cannot advance time");
                const auto span = stop - begin;
                // A fixed q grid has no constant physical spacing. Provide an
                // explicit in-segment stencil for every regularized force arc.
                if (regularization_ || static_cast<double>(span) * 1e-9 < intervals * ceiling) {
                    if (span < intervals)
                        throw PropagationError(
                            "HPOP coast has too few distinct epochs for interpolation order");
                    for (std::int64_t i = 1; i < intervals; ++i)
                        support_gates.push_back(begin + span * i / intervals);
                }
                support_gates.push_back(stop);
                begin = stop;
            }
        }
        if (!extra_gates.empty()) {
            support_gates.insert(support_gates.end(), extra_gates.begin(), extra_gates.end());
            support_gates.push_back(duration);
            std::ranges::sort(support_gates);
            support_gates.erase(std::unique(support_gates.begin(), support_gates.end()),
                                support_gates.end());
        }
        std::size_t gate_index = 0;
        std::int64_t elapsed = 0;
        while (elapsed < duration) {
            const auto next =
                reporting_step == 0
                    ? (support_gates.empty() ? duration : support_gates.at(gate_index++))
                    : elapsed + std::min(reporting_step, duration - elapsed);
            const Epoch gate{initial_.epoch.tai_nanoseconds() + sign * next};
            const double until = gate.seconds_since(initial_.epoch);
            if (until == at)
                throw PropagationError("HPOP reporting step is unresolved in relative time");
            const auto accepted = [&](double time, const State &state, bool boundary,
                                      std::optional<Epoch> weather = {}) {
                if (reporting_step && !boundary && time != until)
                    return;
                // Reporting gates and the requested stop retain their exact integer epochs.
                const auto when = time == until ? gate : epoch_at(time);
                const auto previous = arcs.back().back().epoch.tai_nanoseconds();
                if (regularization_ && when.tai_nanoseconds() == previous) {
                    // Transformed nodes can round to the same integer nanosecond.
                    // Keep an exact output/force boundary over the preceding interior
                    // node; otherwise preserve the already published initial/boundary.
                    if (time != until && !boundary)
                        return;
                    if (arcs.back().size() <= 1)
                        throw PropagationError("HPOP output boundary has no distinct epoch");
                    arcs.back().pop_back();
                } else if ((sign > 0 && when.tai_nanoseconds() <= previous) ||
                           (sign < 0 && when.tai_nanoseconds() >= previous)) {
                    throw PropagationError("HPOP accepted ephemeris nodes are not distinct");
                }
                if ((sign > 0 && when.tai_nanoseconds() > end.tai_nanoseconds()) ||
                    (sign < 0 && when.tai_nanoseconds() < end.tai_nanoseconds()))
                    throw PropagationError("HPOP accepted node exceeds the requested coast");
                const auto output = (regularization_ || mitigates_shadow())
                                        ? epoch_output(time, state, when, weather)
                                        : state;
                CartesianState node{when,
                                    OriginId::earth,
                                    FrameId::gcrf,
                                    {output[0], output[1], output[2]},
                                    {output[3], output[4], output[5]}};
                arcs.back().push_back(node);
                if (boundary && when != end)
                    arcs.push_back({node});
            };
            if (regularization_) {
                regular_state = integrate_regularized(std::move(regular_state), until, attempts,
                                                      checkpoint, accepted);
                value = regular_state.cartesian();
            } else if (options_.uses_fixed_step()) {
                auto first_node = static_cast<std::size_t>(
                    std::floor(std::abs(at) / options_.fixed_step_seconds));
                const auto grid = [&](std::size_t n) { return n * options_.fixed_step_seconds; };
                if (first_node && grid(first_node) > std::abs(at))
                    --first_node;
                if (grid(first_node + 1) <= std::abs(at))
                    ++first_node;
                if (options_.integrator == HpopIntegrator::gauss_jackson) {
                    history = gauss_jackson_integrate(history, at, until, first_node, attempts,
                                                      checkpoint, accepted);
                    value = history.state;
                } else {
                    value = integrate_fixed(value, at, until, first_node, attempts, checkpoint,
                                            accepted);
                }
            } else {
                value =
                    integrate(value, at, until, attempts, checkpoint, accepted, &continued_step);
            }
            at = until;
            elapsed = next;
        }
        if (checkpoint)
            checkpoint();
        if (finish < 0) {
            for (auto &arc : arcs)
                std::reverse(arc.begin(), arc.end());
            std::reverse(arcs.begin(), arcs.end());
        }
        return arcs;
    }
    std::vector<OrbitEphemerisArc> ephemeris(Epoch end, std::int64_t reporting_step,
                                             const std::function<void()> &checkpoint,
                                             std::size_t minimum_nodes = 1) const {
        std::size_t attempts = 0;
        auto arcs = ephemeris_once(end, reporting_step, checkpoint, minimum_nodes, {}, attempts);
        if (!mitigates_shadow() || minimum_nodes <= 1 || reporting_step)
            return arcs;
        std::vector<std::int64_t> gates;
        const std::int64_t sign = end.tai_nanoseconds() < initial_.epoch.tai_nanoseconds() ? -1 : 1;
        for (unsigned refinement = 0; refinement < 8; ++refinement) {
            bool complete = true;
            for (const auto &arc : arcs) {
                if (arc.size() >= minimum_nodes)
                    continue;
                complete = false;
                const auto a =
                    sign * (arc.front().epoch.tai_nanoseconds() - initial_.epoch.tai_nanoseconds());
                const auto b =
                    sign * (arc.back().epoch.tai_nanoseconds() - initial_.epoch.tai_nanoseconds());
                const auto first = std::min(a, b), span = std::abs(b - a);
                const auto intervals = static_cast<std::int64_t>(minimum_nodes + 1);
                if (span < intervals)
                    throw PropagationError(
                        "HPOP shadow arc has too few distinct epochs for interpolation order");
                for (std::int64_t i = 1; i < intervals; ++i)
                    gates.push_back(first + span * i / intervals);
            }
            if (complete) {
                const auto interpolated = make_tabulated_orbit(arcs, options_.interpolation);
                // Endpoint interpolation of the non-smooth penumbra needs more
                // than a node-count check, particularly for Lagrange velocities.
                // Check the first/last stencil against independent BS prefixes and
                // refine only failing gaps. These are real integration gates.
                for (const auto &arc : arcs) {
                    for (std::size_t i = 0; i + 1 < arc.size(); ++i) {
                        if (i >= minimum_nodes && i + minimum_nodes + 1 < arc.size())
                            continue;
                        const auto left_ns = arc[i].epoch.tai_nanoseconds();
                        const auto right_ns = arc[i + 1].epoch.tai_nanoseconds();
                        if (right_ns - left_ns < 2)
                            continue;
                        const Epoch middle{left_ns + (right_ns - left_ns) / 2};
                        const double begin = arc[i].epoch.seconds_since(initial_.epoch);
                        const double until = middle.seconds_since(initial_.epoch);
                        if (begin == until)
                            continue;
                        State start;
                        for (std::size_t axis = 0; axis < 3; ++axis) {
                            start[axis] = arc[i].position_m[axis];
                            start[axis + 3] = arc[i].velocity_mps[axis];
                        }
                        auto truth = one_sided_startup(start, begin, until, attempts, checkpoint);
                        truth =
                            epoch_output(until, truth, middle, weather_stage(until, begin, until));
                        const auto approximate = interpolated->state_at(middle, checkpoint);
                        bool resolved = true;
                        for (std::size_t axis = 0; axis < 3; ++axis) {
                            const double p_scale =
                                options_.position_tolerance_m +
                                options_.relative_tolerance * std::abs(truth[axis]);
                            const double v_scale =
                                options_.velocity_tolerance_mps +
                                options_.relative_tolerance * std::abs(truth[axis + 3]);
                            resolved = resolved &&
                                       std::abs(approximate.position_m[axis] - truth[axis]) <=
                                           .1 * p_scale &&
                                       std::abs(approximate.velocity_mps[axis] - truth[axis + 3]) <=
                                           .1 * v_scale;
                        }
                        if (!resolved) {
                            complete = false;
                            gates.push_back(sign * (middle.tai_nanoseconds() -
                                                    initial_.epoch.tai_nanoseconds()));
                        }
                    }
                }
            }
            if (complete)
                return arcs;
            // A discovery pass only supplies interior output gates. Contact times
            // are located again on the actual refined integration, never imposed
            // from a different trajectory or filled by cross-boundary interpolation.
            arcs = ephemeris_once(end, 0, checkpoint, minimum_nodes, gates, attempts);
        }
        throw PropagationError(
            "HPOP shadow interpolation did not converge within eight refinements");
    }
    CartesianState state_at(Epoch epoch) const override { return state_at(epoch, {}); }
    CartesianState state_at(Epoch epoch, const std::function<void()> &checkpoint) const override {
        if (checkpoint)
            checkpoint();
        const double t = epoch.seconds_since(initial_.epoch);
        if (epoch == initial_.epoch)
            return initial_;
        validate_interval(epoch);
        if (regularization_) {
            // Bound epoch conversion to this query, including its exact integer-ns
            // endpoint. Recompute direct queries; interpolated providers prepare and
            // cache a full declared coast instead of sharing partial query histories.
            const HpopSystem coast(initial_, options_, environment_, epoch);
            std::size_t attempts = 0;
            const auto integrated =
                coast
                    .integrate_regularized(coast.regularization_->initial_state(), t, attempts,
                                           checkpoint)
                    .cartesian();
            const auto value =
                coast.epoch_output(t, integrated, epoch, coast.weather_stage(t, 0, t));
            return {epoch,
                    OriginId::earth,
                    FrameId::gcrf,
                    {value[0], value[1], value[2]},
                    {value[3], value[4], value[5]}};
        }
        if (options_.uses_fixed_step()) {
            std::size_t attempts = 0;
            const auto value = options_.integrator == HpopIntegrator::gauss_jackson
                                   ? gauss_jackson_at(t, attempts, checkpoint)
                                   : fixed_at(t, attempts, checkpoint);
            return {epoch,
                    OriginId::earth,
                    FrameId::gcrf,
                    {value[0], value[1], value[2]},
                    {value[3], value[4], value[5]}};
        }
        // Caching must not impose an undocumented maximum integration step. Keep
        // full epoch-anchored blocks at least as wide as the configured ceiling.
        // Partial outputs are integrated separately and never write into this cache.
        const double interval = std::max(checkpoint_seconds, options_.max_step_seconds);
        const double sign = t < 0 ? -1 : 1;
        const auto grid = [=](std::size_t block) { return block * interval; };
        auto index = static_cast<std::size_t>(std::floor(std::abs(t) / interval));
        if (index > 0 && grid(index) > std::abs(t))
            --index;
        if (grid(index + 1) <= std::abs(t))
            ++index;
        State value;
        double at;
        std::size_t attempts = 0;
        {
            const std::lock_guard lock{mutex_};
            auto &cache = t < 0 ? backward_ : forward_;
            while (cache.size() <= index) {
                const auto block = cache.size() - 1;
                const auto next = integrate(cache.back(), sign * grid(block),
                                            sign * grid(block + 1), attempts, checkpoint);
                cache.push_back(next);
            }
            value = cache[index];
            at = sign * grid(index);
        }
        value = integrate(value, at, t, attempts, checkpoint);
        return {epoch,
                OriginId::earth,
                FrameId::gcrf,
                {value[0], value[1], value[2]},
                {value[3], value[4], value[5]}};
    }
};
using Hpop = HpopSystem<>;
class InterpolatedHpop final : public IStateProvider {
  public:
    InterpolatedHpop(CartesianState initial, Epoch first, Epoch last, HpopOptions options,
                     HpopEnvironment environment, std::int64_t reporting)
        : initial_(initial), first_(first), last_(last), options_(std::move(options)),
          environment_(std::move(environment)), reporting_(reporting) {
        if (first.tai_nanoseconds() > last.tai_nanoseconds())
            throw std::invalid_argument("HPOP interpolation interval must be ordered");
        if (reporting < 0 || reporting > 86400'000'000'000LL)
            throw std::invalid_argument("HPOP reporting step must be 0..86400 seconds");
        Hpop(initial_, options_, environment_).validate_range(first, last);
    }
    CartesianState state_at(Epoch epoch) const override { return state_at(epoch, {}); }
    CartesianState state_at(Epoch epoch, const std::function<void()> &checkpoint) const override {
        if (checkpoint)
            checkpoint();
        if (epoch.tai_nanoseconds() < first_.tai_nanoseconds() ||
            epoch.tai_nanoseconds() > last_.tai_nanoseconds())
            throw PropagationError("Epoch is outside the HPOP interpolation interval");
        if (first_ == initial_.epoch && last_ == initial_.epoch)
            return initial_;
        return prepared(checkpoint)->state_at(epoch, checkpoint);
    }
    std::vector<Epoch> discontinuities() const override { return discontinuities({}); }
    std::vector<Epoch> discontinuities(const std::function<void()> &checkpoint) const override {
        if (checkpoint)
            checkpoint();
        if (first_ == initial_.epoch && last_ == initial_.epoch)
            return {};
        auto result = prepared(checkpoint)->discontinuities(checkpoint);
        std::erase_if(result, [&](Epoch e) {
            return e.tai_nanoseconds() < first_.tai_nanoseconds() ||
                   e.tai_nanoseconds() > last_.tai_nanoseconds();
        });
        return result;
    }

  private:
    std::shared_ptr<const IStateProvider> prepared(const std::function<void()> &checkpoint) const {
        std::lock_guard lock(mutex_);
        if (checkpoint)
            checkpoint();
        if (!prepared_) {
            std::vector<OrbitEphemerisArc> arcs;
            const auto count = static_cast<std::size_t>(options_.interpolation.method ==
                                                                OrbitInterpolationMethod::hermite
                                                            ? (options_.interpolation.order + 1) / 2
                                                            : options_.interpolation.order + 1);
            const auto append = [&](Epoch end) {
                auto part = Hpop(initial_, options_, environment_, end)
                                .ephemeris(end, reporting_, checkpoint, count);
                for (auto &arc : part)
                    arcs.push_back(std::move(arc));
            };
            if (first_.tai_nanoseconds() < initial_.epoch.tai_nanoseconds())
                append(first_);
            if (last_.tai_nanoseconds() > initial_.epoch.tai_nanoseconds())
                append(last_);
            auto candidate =
                make_tabulated_orbit(std::move(arcs), options_.interpolation, checkpoint);
            if (checkpoint)
                checkpoint();
            prepared_ = std::move(candidate);
        }
        return prepared_;
    }
    CartesianState initial_;
    Epoch first_, last_;
    HpopOptions options_;
    HpopEnvironment environment_;
    std::int64_t reporting_;
    mutable std::mutex mutex_;
    mutable std::shared_ptr<const IStateProvider> prepared_;
};
} // namespace
void HpopEnvironment::validate_ephemeris_interval(const HpopOptions &options, Epoch first,
                                                  Epoch last) const {
    if (!options.uses_ephemeris())
        return;
    if (!ephemeris)
        throw std::invalid_argument("Enabled HPOP forces require selected ephemeris data");
    ephemeris->validate_interval(first, last, options.required_ephemeris_bodies());
    if (options.srp.enabled && options.srp.shadow_model != SrpShadowModel::none &&
        !options.srp.additional_eclipsing_bodies.empty()) {
        std::vector<ThirdBody> bodies;
        for (const auto &body : options.srp.additional_eclipsing_bodies)
            bodies.push_back(body.body);
        ephemeris->validate_eclipsing_body_interval(first, last, bodies);
    }
    if ((options.drag.enabled && options.drag.use_apparent_sun_position) ||
        (options.srp.enabled &&
         options.srp.sun_position_method == SrpSunPositionMethod::apparent_to_true_cb))
        ephemeris->validate_apparent_sun_interval(first, last);
    if (options.srp.enabled && options.srp.sun_position_method == SrpSunPositionMethod::apparent)
        ephemeris->validate_spacecraft_apparent_sun_interval(first, last);
}
namespace {
double compatible_area(double mass, double ratio) {
    positive(mass, "Satellite mass must be finite and positive in kg");
    if (!std::isfinite(ratio) || ratio < 0 || ratio > 9999)
        throw std::invalid_argument("Spherical area/mass ratio must be within 0..9999 m^2/kg");
    const double area = mass * ratio;
    if (!std::isfinite(area) || (ratio > 0 && area == 0))
        throw std::invalid_argument(
            "Satellite mass times area/mass ratio is not representable in m^2");
    return area;
}
} // namespace

double HpopOptions::drag_area_m2() const {
    return compatible_area(satellite_mass_kg, drag.area_mass_ratio_m2_per_kg);
}
double HpopOptions::srp_area_m2() const {
    return compatible_area(satellite_mass_kg, srp.area_mass_ratio_m2_per_kg);
}
void validate_hpop_options(const HpopOptions &o) {
    validate_orbit_interpolation(o.interpolation);
    (void)o.drag_area_m2();
    (void)o.srp_area_m2();
    validate_solid_tide_options(o.solid_tides);
    validate_ocean_tide_options(o.ocean_tides);
    validate_srp_options(o.srp);
    validate_drag_options(o.drag);
    if (o.gravity_model.size() > 64 ||
        !std::all_of(o.gravity_model.begin(), o.gravity_model.end(), [](char ch) {
            return (ch >= 'a' && ch <= 'z') || (ch >= '0' && ch <= '9') || ch == '-';
        }))
        throw std::invalid_argument("Invalid gravity model selection");
    positive(o.gravity_mu, "Gravity GM must be finite and positive");
    if (o.ephemeris_model.empty() || o.ephemeris_model.size() > 64 ||
        !std::all_of(o.ephemeris_model.begin(), o.ephemeris_model.end(), [](char ch) {
            return (ch >= 'a' && ch <= 'z') || (ch >= '0' && ch <= '9') || ch == '-';
        }))
        throw std::invalid_argument("Invalid ephemeris model selection");
    for (auto id : third_body_list) {
        const auto &body = o.third_body(id);
        if (body.source != ThirdBodyGravitySource::cb_file &&
            body.source != ThirdBodyGravitySource::jpl_de &&
            body.source != ThirdBodyGravitySource::user_specified)
            throw std::invalid_argument("Unsupported third-body gravity source");
        if (body.user_mu)
            positive(*body.user_mu, "Third-body GM must be finite and positive");
        if (body.cb) {
            positive(body.cb->mu, "Cb file GM must be finite and positive");
            if (body.cb->name.empty() || body.cb->name.size() > 256 ||
                body.cb->content_sha256.size() != 64 ||
                !std::all_of(
                    body.cb->content_sha256.begin(), body.cb->content_sha256.end(),
                    [](char ch) { return (ch >= '0' && ch <= '9') || (ch >= 'a' && ch <= 'f'); }) ||
                body.cb->body != id)
                throw std::invalid_argument("Invalid Cb file provenance");
        }
        if (body.enabled && body.source == ThirdBodyGravitySource::cb_file && !body.cb)
            throw std::invalid_argument("Cb file gravity requires imported constants");
        if (body.enabled && body.source == ThirdBodyGravitySource::user_specified && !body.user_mu)
            throw std::invalid_argument("User Specified gravity requires a GM value");
    }
    const int limit = o.gravity_model.empty() ? egm96_max_degree : gravity_max_degree;
    if (o.gravity_degree < 0 || o.gravity_degree == 1 || o.gravity_degree > limit ||
        o.gravity_order < 0 || o.gravity_order > o.gravity_degree)
        throw std::invalid_argument(
            "HPOP gravity degree must be 0 or 2..model limit; order must be 0..degree");
    if (o.integrator != HpopIntegrator::rkf78 && o.integrator != HpopIntegrator::rk4 &&
        o.integrator != HpopIntegrator::rkv89 && o.integrator != HpopIntegrator::bulirsch_stoer &&
        o.integrator != HpopIntegrator::gauss_jackson)
        throw std::invalid_argument("Unsupported HPOP integrator");
    if (o.step_control != HpopStepControl::relative_error &&
        o.step_control != HpopStepControl::fixed)
        throw std::invalid_argument("Unsupported HPOP step size control");
    if (o.correction != HpopCorrection::full && o.correction != HpopCorrection::pseudo)
        throw std::invalid_argument("Unsupported HPOP predictor corrector scheme");
    if (!std::isfinite(o.regularization_exponent) || o.regularization_exponent <= 0 ||
        o.regularization_exponent > 10)
        throw std::invalid_argument("HPOP regularization exponent must be in (0, 10]");
    if (o.steps_per_orbit <= 0)
        throw std::invalid_argument("HPOP steps per orbit must be positive");
    positive(o.position_tolerance_m, "HPOP position tolerance must be finite and positive");
    positive(o.velocity_tolerance_mps, "HPOP velocity tolerance must be finite and positive");
    if (!std::isfinite(o.relative_tolerance) || o.relative_tolerance < 1e-14 ||
        o.relative_tolerance > 1e-3)
        throw std::invalid_argument("HPOP relative tolerance must be between 1e-14 and 1e-3");
    if (!std::isfinite(o.min_step_seconds) || !std::isfinite(o.max_step_seconds) ||
        o.min_step_seconds < 1e-6 || o.max_step_seconds < o.min_step_seconds ||
        o.max_step_seconds > hpop_max_step_seconds)
        throw std::invalid_argument(
            "HPOP adaptive steps require 1e-6 <= min <= max <= 86400 seconds");
    if (!std::isfinite(o.fixed_step_seconds) || o.fixed_step_seconds < 1e-6 ||
        o.fixed_step_seconds > hpop_max_step_seconds)
        throw std::invalid_argument("HPOP fixed step must be between 1e-6 and 86400 seconds");
    if (!std::isfinite(o.minimum_altitude_m) || o.minimum_altitude_m < 0)
        throw std::invalid_argument("HPOP minimum altitude must be finite and nonnegative");
}
HpopVariationalState propagate_hpop_variational(CartesianState initial, Epoch end,
                                                HpopOptions options, HpopEnvironment environment,
                                                HpopVariationalOptions variational,
                                                const std::function<void()> &checkpoint) {
    if (checkpoint)
        checkpoint();
    return HpopSystem<true>(initial, std::move(options), std::move(environment), end,
                            std::move(variational), checkpoint)
        .variational_at(end, checkpoint);
}
void detail::sample_hpop_variational(
    CartesianState initial, const std::vector<Epoch> &times, HpopOptions options,
    HpopEnvironment environment, HpopVariationalOptions variational,
    const std::function<void(std::size_t, const HpopVariationalState &)> &sample,
    const std::function<void()> &checkpoint) {
    if (checkpoint)
        checkpoint();
    if (times.empty())
        return;
    std::vector<std::size_t> indices(times.size());
    std::iota(indices.begin(), indices.end(), 0);
    std::stable_sort(indices.begin(), indices.end(), [&](std::size_t a, std::size_t b) {
        return times[a].tai_nanoseconds() < times[b].tai_nanoseconds();
    });
    const auto split = std::lower_bound(
        indices.begin(), indices.end(), initial.epoch,
        [&](std::size_t i, Epoch t) { return times[i].tai_nanoseconds() < t.tai_nanoseconds(); });
    const auto negative_count = static_cast<std::size_t>(split - indices.begin());
    std::unique_ptr<HpopSystem<true>> backward, forward;
    // Preflight both directions before invoking any consumer. Each instance keeps
    // stage evaluations inside its full requested coast, including the initial epoch.
    if (negative_count) {
        backward = std::make_unique<HpopSystem<true>>(
            initial, options, environment, times[indices.front()], variational, checkpoint);
        backward->validate_range(times[indices.front()], initial.epoch);
    }
    if (split != indices.end()) {
        forward = std::make_unique<HpopSystem<true>>(
            initial, options, environment, times[indices.back()], variational, checkpoint);
        forward->validate_range(initial.epoch, times[indices.back()]);
    }
    std::reverse(indices.begin(), split);
    if (backward)
        backward->sample_variational(times, std::span{indices}.first(negative_count), sample,
                                     checkpoint);
    if (forward)
        forward->sample_variational(times, std::span{indices}.subspan(negative_count), sample,
                                    checkpoint);
}
std::shared_ptr<const IStateProvider> make_hpop(CartesianState initial, HpopOptions options,
                                                HpopEnvironment environment) {
    return std::make_shared<Hpop>(initial, options, std::move(environment));
}
void HpopEnvironment::validate_weather_interval(const DragOptions &drag, Epoch first,
                                                Epoch last) const {
    if (!drag.enabled || drag.weather_model.empty())
        return;
    if (!weather || !time)
        throw std::invalid_argument("File drag requires selected weather and UTC providers");
    weather->validate_interval(first, last, *time, drag.atmosphere, drag.geomagnetic_update,
                               drag.geomagnetic_source, sun_distance(ephemeris.get()));
    if (drag.atmosphere == AtmosphereModel::jacchia_roberts &&
        drag.low_altitude_model == LowAltitudeDensityModel::nrlmsise00)
        weather->validate_interval(first, last, *time, AtmosphereModel::nrlmsise00,
                                   drag.geomagnetic_update, drag.geomagnetic_source,
                                   sun_distance(ephemeris.get()));
}
std::vector<OrbitEphemerisArc> propagate_hpop_ephemeris(CartesianState initial, Epoch end,
                                                        HpopOptions options,
                                                        HpopEnvironment environment,
                                                        std::int64_t reporting_step_nanoseconds,
                                                        const std::function<void()> &checkpoint) {
    if (checkpoint)
        checkpoint();
    return Hpop(initial, std::move(options), std::move(environment), end)
        .ephemeris(end, reporting_step_nanoseconds, checkpoint);
}
std::shared_ptr<const IStateProvider>
make_hpop_interpolated(CartesianState initial, Epoch first, Epoch last, HpopOptions options,
                       HpopEnvironment environment, std::int64_t reporting_step_nanoseconds) {
    return std::make_shared<InterpolatedHpop>(initial, first, last, std::move(options),
                                              std::move(environment), reporting_step_nanoseconds);
}
} // namespace orbelle::core
