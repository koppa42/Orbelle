#include <algorithm>
#include <chrono>
#include <cmath>
#include <limits>
#include <mutex>
#include <orbitforge/core/propagation/hpop.hpp>
#include <tuple>
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
    // File geomagnetism is piecewise constant in UTC. Stop at every update,
    // including the separately lagged JR and low-altitude MSIS windows.
    std::optional<double> weather_boundary(double t, double end) const {
        const auto &d = options_.drag;
        if (!d.enabled || d.weather_model.empty())
            return {};
        const bool forward = end > t;
        const auto at = epoch_at(t);
        const auto until = epoch_at(end);
        const int period = d.geomagnetic_update == GeomagneticUpdate::daily ? 24 : 3;
        const auto boundary = [&](AtmosphereModel model) {
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
            const auto instant = [&](int h) {
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
                    return end;
                const auto e = environment_.time->from_utc(boundary_utc);
                if (e.tai_nanoseconds() > std::numeric_limits<std::int64_t>::max() - lag)
                    throw PropagationError("Weather boundary exceeds the Epoch range");
                return Epoch{e.tai_nanoseconds() + lag}.seconds_since(initial_.epoch);
            };
            auto next = instant(hour);
            // Compare in the integrator's relative time, so rounding to nanoseconds
            // at an accepted boundary cannot create a zero-length next step.
            if (forward ? next <= t : next >= t)
                next = instant(hour + (forward ? period : -period));
            return next;
        };
        auto next = boundary(d.atmosphere);
        if (d.atmosphere == AtmosphereModel::jacchia_roberts &&
            d.low_altitude_model == LowAltitudeDensityModel::nrlmsise00) {
            const auto low = boundary(AtmosphereModel::nrlmsise00);
            next = forward ? std::min(next, low) : std::max(next, low);
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
    State derivative(double t, const State &s, std::optional<Epoch> weather_at = {}) const {
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
        if (options_.uses_third_bodies()) {
            const auto epoch = epoch_at(t);
            const auto &info = environment_.ephemeris->data_info();
            const auto add = [&](ThirdBody body, double mu) {
                const auto force =
                    third_body_acceleration(r, environment_.ephemeris->position(body, epoch), mu);
                for (int i = 0; i < 3; ++i)
                    acceleration[i] += force[i];
            };
            for (auto body : third_body_list)
                if (options_.third_body(body).enabled)
                    add(body, third_body_mu(options_.third_body(body), body, info));
        }
        if (options_.srp.enabled) {
            const auto epoch = epoch_at(t);
            std::optional<std::array<double, 3>> moon;
            if (options_.srp.moon_shadow && options_.srp.shadow_model != SrpShadowModel::none)
                moon = environment_.ephemeris->position(ThirdBody::moon, epoch);
            auto sun = [&] {
                switch (options_.srp.sun_position_method) {
                case SrpSunPositionMethod::geometric:
                    return environment_.ephemeris->position(ThirdBody::sun, epoch);
                case SrpSunPositionMethod::apparent_to_true_cb:
                    return environment_.ephemeris->apparent_sun_from_earth(epoch);
                case SrpSunPositionMethod::apparent: {
                    auto received = environment_.ephemeris->apparent_sun_from_spacecraft(
                        epoch, r, {s[3], s[4], s[5]});
                    for (std::size_t i = 0; i < 3; ++i)
                        received[i] += r[i]; // Geometry evaluator takes a geocentric Sun endpoint.
                    return received;
                }
                }
                throw std::invalid_argument("Unsupported SRP Sun position method");
            }();
            const auto force = evaluate_srp(r, sun, options_.srp, moon);
            for (int i = 0; i < 3; ++i)
                acceleration[i] += force.acceleration_mps2[i];
        }
        if (options_.drag.enabled) {
            const auto epoch = epoch_at(t);
            const auto transform =
                environment_.frames->gcrf_to_itrf_state(epoch, environment_.eop->at(epoch));
            const auto fixed =
                transform.apply({epoch, OriginId::earth, FrameId::gcrf, r, {s[3], s[4], s[5]}});
            const auto sun = options_.drag.use_apparent_sun_position
                                 ? environment_.ephemeris->apparent_sun_from_earth(epoch)
                                 : environment_.ephemeris->position(ThirdBody::sun, epoch);
            std::optional<SpaceWeatherSample> weather, lower;
            if (!options_.drag.weather_model.empty()) {
                const auto &d = options_.drag;
                weather = environment_.weather->sample(weather_at.value_or(epoch),
                                                       *environment_.time, d.atmosphere,
                                                       d.geomagnetic_update, d.geomagnetic_source);
                if (d.atmosphere == AtmosphereModel::jacchia_roberts &&
                    d.low_altitude_model == LowAltitudeDensityModel::nrlmsise00)
                    lower = environment_.weather->sample(
                        weather_at.value_or(epoch), *environment_.time, AtmosphereModel::nrlmsise00,
                        d.geomagnetic_update, d.geomagnetic_source);
            }
            const auto force = evaluate_drag(
                fixed.position_m, fixed.velocity_mps, transform.rotation.apply_position(sun),
                environment_.time->to_utc(epoch), options_.drag, weather, lower);
            const auto inertial = transform.rotation.inverse_position(force.acceleration_itrf_mps2);
            for (int i = 0; i < 3; ++i)
                acceleration[i] += inertial[i];
        }
        return {s[3], s[4], s[5], acceleration[0], acceleration[1], acceleration[2]};
    }
    State rk4(double t, const State &s, double h) const {
        const auto k1 = derivative(t, s, weather_stage(t, t, t + h));
        State tmp{};
        for (std::size_t i = 0; i < 6; ++i)
            tmp[i] = s[i] + h * 0.5 * k1[i];
        const auto k2 = derivative(t + h * 0.5, tmp, weather_stage(t + h * 0.5, t, t + h));
        for (std::size_t i = 0; i < 6; ++i)
            tmp[i] = s[i] + h * 0.5 * k2[i];
        const auto k3 = derivative(t + h * 0.5, tmp, weather_stage(t + h * 0.5, t, t + h));
        for (std::size_t i = 0; i < 6; ++i)
            tmp[i] = s[i] + h * k3[i];
        const auto k4 = derivative(t + h, tmp, weather_stage(t + h, t, t + h));
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
            k[stage] =
                derivative(t + c[stage] * h, value, weather_stage(t + c[stage] * h, t, t + h));
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
            double magnitude = std::min(step, remaining);
            const auto boundary = weather_boundary(t, end);
            if (boundary)
                magnitude = std::min(magnitude, std::abs(*boundary - t));
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
        environment_.validate_ephemeris_interval(options_, initial.epoch, initial.epoch);
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
        if ((options.gravity_degree > 0 || options.drag.enabled) &&
            (!environment_.eop || !environment_.frames))
            throw std::invalid_argument(
                "HPOP noncentral gravity and drag require explicit EOP and frame providers");
        if ((options.gravity_degree > 0 || options.drag.enabled) &&
            !eop_covers(environment_.eop->data_info(), initial.epoch))
            throw EopCoverageError("HPOP initial epoch is outside selected EOP coverage");
        if (options.drag.enabled) {
            if (!environment_.time || environment_.time->data_info().sha256 !=
                                          environment_.eop->data_info().time_data.sha256)
                throw std::invalid_argument(
                    "Atmospheric drag requires the EOP's confirmed UTC provider");
            (void)environment_.time->to_utc(initial.epoch);
        }
        environment_.validate_weather_interval(options_.drag, initial.epoch, initial.epoch);
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
        environment_.validate_weather_interval(
            options_.drag,
            Epoch{std::min(epoch.tai_nanoseconds(), initial_.epoch.tai_nanoseconds())},
            Epoch{std::max(epoch.tai_nanoseconds(), initial_.epoch.tai_nanoseconds())});
        environment_.validate_ephemeris_interval(options_, t < 0 ? epoch : initial_.epoch,
                                                 t < 0 ? initial_.epoch : epoch);
        if ((options_.gravity_degree > 0 || options_.drag.enabled) &&
            !eop_covers(environment_.eop->data_info(), epoch))
            throw EopCoverageError(
                "HPOP integration requires EOP covering the initial and query epochs");
        if (options_.drag.enabled)
            (void)environment_.time->to_utc(epoch);
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
void HpopEnvironment::validate_ephemeris_interval(const HpopOptions &options, Epoch first,
                                                  Epoch last) const {
    if (!options.uses_ephemeris())
        return;
    if (!ephemeris)
        throw std::invalid_argument("Enabled HPOP forces require selected ephemeris data");
    ephemeris->validate_interval(first, last, options.required_ephemeris_bodies());
    if ((options.drag.enabled && options.drag.use_apparent_sun_position) ||
        (options.srp.enabled &&
         options.srp.sun_position_method == SrpSunPositionMethod::apparent_to_true_cb))
        ephemeris->validate_apparent_sun_interval(first, last);
    if (options.srp.enabled && options.srp.sun_position_method == SrpSunPositionMethod::apparent)
        ephemeris->validate_spacecraft_apparent_sun_interval(first, last);
}
void validate_hpop_options(const HpopOptions &o) {
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
void HpopEnvironment::validate_weather_interval(const DragOptions &drag, Epoch first,
                                                Epoch last) const {
    if (!drag.enabled || drag.weather_model.empty())
        return;
    if (!weather || !time)
        throw std::invalid_argument("File drag requires selected weather and UTC providers");
    weather->validate_interval(first, last, *time, drag.atmosphere, drag.geomagnetic_update,
                               drag.geomagnetic_source);
    if (drag.atmosphere == AtmosphereModel::jacchia_roberts &&
        drag.low_altitude_model == LowAltitudeDensityModel::nrlmsise00)
        weather->validate_interval(first, last, *time, AtmosphereModel::nrlmsise00,
                                   drag.geomagnetic_update, drag.geomagnetic_source);
}
} // namespace orbitforge::core
