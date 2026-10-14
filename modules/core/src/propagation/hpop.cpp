#include <algorithm>
#include <cmath>
#include <limits>
#include <mutex>
#include <orbitforge/core/propagation/hpop.hpp>
#include <vector>

namespace orbitforge::core {
namespace {
using State = std::array<double, 6>;
// Fehlberg, NASA TR R-287 (1968), Table XI. Eighth order accepted solution,
// embedded seventh order error estimate; all stage epochs stay within a step.
constexpr std::array<double, 13> c{0,       2.0 / 27, 1.0 / 9, 1.0 / 6, 5.0 / 12, 0.5, 5.0 / 6,
                                   1.0 / 6, 2.0 / 3,  1.0 / 3, 1,       0,        1};
constexpr double a[13][13] = {
    {},
    {2.0 / 27},
    {1.0 / 36, 1.0 / 12},
    {1.0 / 24, 0, 1.0 / 8},
    {5.0 / 12, 0, -25.0 / 16, 25.0 / 16},
    {1.0 / 20, 0, 0, 1.0 / 4, 1.0 / 5},
    {-25.0 / 108, 0, 0, 125.0 / 108, -65.0 / 27, 125.0 / 54},
    {31.0 / 300, 0, 0, 0, 61.0 / 225, -2.0 / 9, 13.0 / 900},
    {2, 0, 0, -53.0 / 6, 704.0 / 45, -107.0 / 9, 67.0 / 90, 3},
    {-91.0 / 108, 0, 0, 23.0 / 108, -976.0 / 135, 311.0 / 54, -19.0 / 60, 17.0 / 6, -1.0 / 12},
    {2383.0 / 4100, 0, 0, -341.0 / 164, 4496.0 / 1025, -301.0 / 82, 2133.0 / 4100, 45.0 / 82,
     45.0 / 164, 18.0 / 41},
    {3.0 / 205, 0, 0, 0, 0, -6.0 / 41, -3.0 / 205, -3.0 / 41, 3.0 / 41, 6.0 / 41},
    {-1777.0 / 4100, 0, 0, -341.0 / 164, 4496.0 / 1025, -289.0 / 82, 2193.0 / 4100, 51.0 / 82,
     33.0 / 164, 12.0 / 41, 0, 1}};
constexpr std::array<double, 13> b{
    0, 0, 0, 0, 0, 34.0 / 105, 9.0 / 35, 9.0 / 35, 9.0 / 280, 9.0 / 280, 0, 41.0 / 840, 41.0 / 840};
constexpr double checkpoint_seconds = 300;
constexpr std::size_t max_attempts = 200000;
void positive(double v, const char *message) {
    if (!std::isfinite(v) || v <= 0)
        throw std::invalid_argument(message);
}
class Hpop final : public IStateProvider {
    CartesianState initial_;
    HpopOptions options_;
    HpopEnvironment environment_;
    // Caches only full, epoch-anchored integration blocks. Partial queries cannot
    // alter the solution used by later queries. Both directions start at t=0.
    mutable std::mutex mutex_;
    mutable std::vector<State> forward_, backward_;
    Epoch epoch_at(double seconds) const {
        const auto offset = static_cast<std::int64_t>(std::llround(seconds * 1e9));
        const auto start = initial_.epoch.tai_nanoseconds();
        if ((offset > 0 && start > std::numeric_limits<std::int64_t>::max() - offset) ||
            (offset < 0 && start < std::numeric_limits<std::int64_t>::min() - offset))
            throw PropagationError("HPOP stage epoch exceeds the representable time range");
        return Epoch{start + offset};
    }
    State derivative(double t, const State &s) const {
        for (double value : s)
            if (!std::isfinite(value))
                throw PropagationError("HPOP encountered a nonfinite state");
        const std::array r{s[0], s[1], s[2]};
        if (std::hypot(r[0], r[1], r[2]) <=
            environment_.gravity->info().radius_m + options_.minimum_altitude_m)
            throw PropagationError("HPOP reached its minimum geocentric altitude");
        std::array<double, 3> acceleration;
        if (options_.gravity_degree == 0) {
            acceleration = environment_.gravity->acceleration(r, 0, 0);
        } else {
            const auto epoch = epoch_at(t);
            const auto rotation =
                environment_.frames->gcrf_to_itrf(epoch, environment_.eop->at(epoch).sample);
            acceleration = rotation.inverse_position(environment_.gravity->acceleration(
                rotation.apply_position(r), options_.gravity_degree, options_.gravity_order));
        }
        return {s[3], s[4], s[5], acceleration[0], acceleration[1], acceleration[2]};
    }
    State rk4(double t, const State &s, double h) const {
        const auto k1 = derivative(t, s);
        State tmp{};
        for (std::size_t i = 0; i < 6; ++i)
            tmp[i] = s[i] + h * 0.5 * k1[i];
        const auto k2 = derivative(t + h * 0.5, tmp);
        for (std::size_t i = 0; i < 6; ++i)
            tmp[i] = s[i] + h * 0.5 * k2[i];
        const auto k3 = derivative(t + h * 0.5, tmp);
        for (std::size_t i = 0; i < 6; ++i)
            tmp[i] = s[i] + h * k3[i];
        const auto k4 = derivative(t + h, tmp);
        for (std::size_t i = 0; i < 6; ++i)
            tmp[i] = s[i] + h / 6 * (k1[i] + 2 * k2[i] + 2 * k3[i] + k4[i]);
        return tmp;
    }
    std::pair<State, double> rkf78(double t, const State &s, double h) const {
        std::array<State, 13> k;
        for (std::size_t stage = 0; stage < 13; ++stage) {
            auto value = s;
            for (std::size_t i = 0; i < 6; ++i) {
                double sum = 0;
                for (std::size_t j = 0; j < stage; ++j)
                    sum += a[stage][j] * k[j][i];
                value[i] += h * sum;
            }
            k[stage] = derivative(t + c[stage] * h, value);
        }
        State result = s;
        double error = 0;
        for (std::size_t i = 0; i < 6; ++i) {
            double sum = 0;
            for (std::size_t j = 0; j < 13; ++j)
                sum += b[j] * k[j][i];
            result[i] += h * sum;
            const double estimate =
                std::abs(h * (41.0 / 840) * (k[11][i] + k[12][i] - k[0][i] - k[10][i]));
            const double absolute =
                i < 3 ? options_.position_tolerance_m : options_.velocity_tolerance_mps;
            const double scale = absolute + options_.relative_tolerance *
                                                std::max(std::abs(s[i]), std::abs(result[i]));
            error = std::max(error, estimate / scale);
        }
        return {result, error};
    }
    State integrate(State state, double t, double end, std::size_t &attempts,
                    const std::function<void()> &checkpoint) const {
        const bool adaptive = options_.integrator == HpopIntegrator::rkf78;
        double step = adaptive ? options_.max_step_seconds : options_.fixed_step_seconds;
        while (t != end) {
            if (checkpoint)
                checkpoint();
            if (++attempts > max_attempts)
                throw PropagationError("HPOP exceeded 200000 integration attempts in one query");
            const double remaining = std::abs(end - t);
            const double magnitude = std::min(step, remaining);
            const double h = std::copysign(magnitude, end - t);
            if (t + h == t)
                throw PropagationError("HPOP integration step cannot advance time");
            auto [next, error] = adaptive ? rkf78(t, state, h) : std::pair{rk4(t, state, h), 0.0};
            if (!std::isfinite(error) ||
                !std::all_of(next.begin(), next.end(), [](double v) { return std::isfinite(v); }))
                throw PropagationError("HPOP integration produced a nonfinite result");
            if (error <= 1) {
                state = next;
                t = magnitude == remaining ? end : t + h;
                // Validate the accepted endpoint as well as all RK stages.
                (void)derivative(t, state);
            } else if (magnitude <= options_.min_step_seconds) {
                throw PropagationError("HPOP cannot meet tolerance at the minimum step");
            }
            if (adaptive) {
                const double factor =
                    error == 0 ? 4 : std::clamp(0.9 * std::pow(error, -0.125), 0.1, 4.0);
                step = std::clamp(magnitude * factor, options_.min_step_seconds,
                                  options_.max_step_seconds);
            }
        }
        return state;
    }

  public:
    Hpop(CartesianState initial, HpopOptions options, HpopEnvironment environment)
        : initial_(initial), options_(options), environment_(std::move(environment)) {
        validate_hpop_options(options_);
        if (options_.gravity_model.empty())
            environment_.gravity = builtin_egm96();
        if (!environment_.gravity)
            throw std::invalid_argument(
                "Selected gravity model is missing; download or import it in Data Center");
        if (environment_.gravity->info().mu != options_.gravity_mu)
            throw std::invalid_argument(
                "HPOP gravity GM differs from the selected file; update model settings");
        if (initial.origin != OriginId::earth || initial.frame != FrameId::gcrf)
            throw std::invalid_argument("HPOP requires an Earth-centered GCRF initial state");
        if (options.gravity_degree > 0 && (!environment_.eop || !environment_.frames))
            throw std::invalid_argument(
                "HPOP spherical gravity requires explicit EOP and frame providers");
        if (options.gravity_degree > 0 && !eop_covers(environment_.eop->data_info(), initial.epoch))
            throw EopCoverageError("HPOP initial epoch is outside selected EOP coverage");
        State value{};
        for (std::size_t i = 0; i < 3; ++i) {
            value[i] = initial.position_m[i];
            value[i + 3] = initial.velocity_mps[i];
        }
        (void)derivative(0, value);
        forward_.push_back(value);
        backward_.push_back(value);
    }
    CartesianState state_at(Epoch epoch) const override { return state_at(epoch, {}); }
    CartesianState state_at(Epoch epoch, const std::function<void()> &checkpoint) const override {
        if (checkpoint)
            checkpoint();
        const double t = epoch.seconds_since(initial_.epoch);
        if (std::abs(t) > hpop_max_duration_seconds)
            throw PropagationError("HPOP query exceeds 366 days from the initial epoch");
        if (epoch == initial_.epoch)
            return initial_;
        if (options_.gravity_degree > 0 && !eop_covers(environment_.eop->data_info(), epoch))
            throw EopCoverageError(
                "HPOP integration requires EOP covering the initial and query epochs");
        const double sign = t < 0 ? -1 : 1;
        const auto index = static_cast<std::size_t>(std::floor(std::abs(t) / checkpoint_seconds));
        State value;
        double at;
        std::size_t attempts = 0;
        {
            const std::lock_guard lock{mutex_};
            auto &cache = t < 0 ? backward_ : forward_;
            while (cache.size() <= index) {
                const double begin =
                    sign * static_cast<double>(cache.size() - 1) * checkpoint_seconds;
                const auto next = integrate(cache.back(), begin, begin + sign * checkpoint_seconds,
                                            attempts, checkpoint);
                cache.push_back(next);
            }
            value = cache[index];
            at = sign * static_cast<double>(index) * checkpoint_seconds;
        }
        value = integrate(value, at, t, attempts, checkpoint);
        return {epoch,
                OriginId::earth,
                FrameId::gcrf,
                {value[0], value[1], value[2]},
                {value[3], value[4], value[5]}};
    }
};
} // namespace
void validate_hpop_options(const HpopOptions &o) {
    if (o.gravity_model.size() > 64 ||
        !std::all_of(o.gravity_model.begin(), o.gravity_model.end(), [](char ch) {
            return (ch >= 'a' && ch <= 'z') || (ch >= '0' && ch <= '9') || ch == '-';
        }))
        throw std::invalid_argument("Invalid gravity model selection");
    positive(o.gravity_mu, "Gravity GM must be finite and positive");
    const int limit = o.gravity_model.empty() ? egm96_max_degree : gravity_max_degree;
    if (o.gravity_degree < 0 || o.gravity_degree == 1 || o.gravity_degree > limit ||
        o.gravity_order < 0 || o.gravity_order > o.gravity_degree)
        throw std::invalid_argument(
            "HPOP gravity degree must be 0 or 2..model limit; order must be 0..degree");
    if (o.integrator != HpopIntegrator::rkf78 && o.integrator != HpopIntegrator::rk4)
        throw std::invalid_argument("Unsupported HPOP integrator");
    positive(o.position_tolerance_m, "HPOP position tolerance must be finite and positive");
    positive(o.velocity_tolerance_mps, "HPOP velocity tolerance must be finite and positive");
    if (!std::isfinite(o.relative_tolerance) || o.relative_tolerance < 1e-14 ||
        o.relative_tolerance > 1e-3)
        throw std::invalid_argument("HPOP relative tolerance must be between 1e-14 and 1e-3");
    if (!std::isfinite(o.min_step_seconds) || !std::isfinite(o.max_step_seconds) ||
        o.min_step_seconds < 1e-6 || o.max_step_seconds < o.min_step_seconds ||
        o.max_step_seconds > 3600)
        throw std::invalid_argument(
            "HPOP adaptive steps require 1e-6 <= min <= max <= 3600 seconds");
    if (!std::isfinite(o.fixed_step_seconds) || o.fixed_step_seconds < 1e-6 ||
        o.fixed_step_seconds > 3600)
        throw std::invalid_argument("HPOP fixed step must be between 1e-6 and 3600 seconds");
    if (!std::isfinite(o.minimum_altitude_m) || o.minimum_altitude_m < 0)
        throw std::invalid_argument("HPOP minimum altitude must be finite and nonnegative");
}
std::shared_ptr<const IStateProvider> make_hpop(CartesianState initial, HpopOptions options,
                                                HpopEnvironment environment) {
    return std::make_shared<Hpop>(initial, options, std::move(environment));
}
} // namespace orbitforge::core
