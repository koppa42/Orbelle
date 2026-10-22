#include <algorithm>
#include <cmath>
#include <limits>
#include <map>
#include <optional>
#include <orbelle/core/propagation/orbital_events.hpp>
#include <stdexcept>

namespace orbelle::core {
std::string_view orbital_event_name(OrbitalEventKind kind) {
    switch (kind) {
    case OrbitalEventKind::periapsis:
        return "Periapsis";
    case OrbitalEventKind::apoapsis:
        return "Apoapsis";
    case OrbitalEventKind::ascending_node:
        return "Ascending node";
    case OrbitalEventKind::descending_node:
        return "Descending node";
    }
    throw std::invalid_argument("Invalid orbital event kind");
}
namespace {
struct Sample {
    CartesianState state;
    double radial, north;
    bool apsides_defined, nodes_defined;
};
Sample evaluate(CartesianState state, Epoch epoch, double mu) {
    if (state.epoch != epoch || state.origin != OriginId::earth || state.frame != FrameId::gcrf)
        throw std::invalid_argument("Orbital events require matching Earth/GCRF states");
    const auto &p = state.position_m, &v = state.velocity_mps;
    const auto r = std::hypot(p[0], p[1], p[2]), speed = std::hypot(v[0], v[1], v[2]);
    if (!std::isfinite(r) || !std::isfinite(speed) || r <= 0 || speed <= 0)
        throw std::invalid_argument("Orbital events require finite nonzero position and velocity");
    std::array<double, 3> u{}, w{};
    double radial = 0;
    for (std::size_t i = 0; i < 3; ++i) {
        u[i] = p[i] / r;
        w[i] = v[i] / speed;
        radial += u[i] * w[i];
    }
    const std::array h{u[1] * w[2] - u[2] * w[1], u[2] * w[0] - u[0] * w[2],
                       u[0] * w[1] - u[1] * w[0]};
    const auto hn = std::hypot(h[0], h[1], h[2]);
    if (!std::isfinite(hn) || hn <= 1e-14)
        throw std::invalid_argument("Orbital events are undefined for radial trajectories");
    std::array<double, 3> eccentricity{};
    const auto factor = (r / mu * speed) * speed;
    for (std::size_t i = 0; i < 3; ++i)
        eccentricity[i] = factor * (u[i] - radial * w[i]) - u[i];
    const auto e = std::hypot(eccentricity[0], eccentricity[1], eccentricity[2]);
    if (!std::isfinite(e))
        throw std::invalid_argument("Orbital event geometry overflow");
    const bool apsides = e > 1e-8, nodes = std::hypot(h[0], h[1]) / hn > 1e-8;
    return {state, apsides ? radial : 0, nodes ? u[2] : 0, apsides, nodes};
}
} // namespace
OrbitalEvents find_orbital_events(Epoch first, Epoch last, std::int64_t step,
                                  const std::function<CartesianState(Epoch)> &at,
                                  GravitationalParameter parameter, std::int64_t tolerance,
                                  AccessSearchMode mode) {
    const auto begin = first.tai_nanoseconds(), end = last.tai_nanoseconds();
    const auto span = static_cast<std::uint64_t>(end) - static_cast<std::uint64_t>(begin);
    const double mu = parameter.metres_cubed_per_second_squared;
    if (!at || end < begin || step <= 0 || tolerance <= 0 || tolerance > step ||
        !std::isfinite(mu) || mu <= 0 ||
        (mode != AccessSearchMode::fixed_grid && mode != AccessSearchMode::extremum_assisted) ||
        span > static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max()) ||
        span / static_cast<std::uint64_t>(step) + (span % static_cast<std::uint64_t>(step) != 0) >=
            200000)
        throw std::invalid_argument(
            "Invalid orbital event interval, step, tolerance or mu (200000 checks maximum)");
    OrbitalEvents result;
    std::map<std::int64_t, Sample> cache;
    const auto sample = [&](std::int64_t t) -> const Sample & {
        if (const auto found = cache.find(t); found != cache.end())
            return found->second;
        if (cache.size() >= 2'000'000)
            throw std::invalid_argument(
                "Orbital events exceed 2000000 evaluations; shorten the interval");
        const auto value = evaluate(at(Epoch{t}), Epoch{t}, mu);
        result.near_circular |= !value.apsides_defined;
        result.near_equatorial |= !value.nodes_defined;
        return cache.emplace(t, value).first->second;
    };
    sample(begin);
    if (begin == end)
        return result;
    for (const bool nodes : {false, true}) {
        std::map<std::int64_t, double> values;
        const auto f = [&](Epoch t) {
            const auto &s = sample(t.tai_nanoseconds());
            const auto value = nodes ? s.north : s.radial;
            values.emplace(t.tai_nanoseconds(), value);
            return value;
        };
        f(first);
        for (auto left = begin; left < end;) {
            const auto right = end - left <= step ? end : left + step;
            f(Epoch{right});
            if (mode == AccessSearchMode::extremum_assisted) {
                (void)find_access_extremum(Epoch{left}, Epoch{right}, step, f, true, tolerance);
                (void)find_access_extremum(Epoch{left}, Epoch{right}, step, f, false, tolerance);
            }
            left = right;
        }
        const auto emit = [&](std::int64_t t, bool increasing) {
            const auto &s = sample(t);
            if (!(nodes ? s.nodes_defined : s.apsides_defined))
                return;
            const auto kind =
                nodes ? (increasing ? OrbitalEventKind::ascending_node
                                    : OrbitalEventKind::descending_node)
                      : (increasing ? OrbitalEventKind::periapsis : OrbitalEventKind::apoapsis);
            result.events.push_back({kind, s.state});
        };
        // Zero runs are only events when isolated and crossed. A plateau or a
        // tangential contact cannot define an apsis/node crossing.
        const auto discovered = values;
        std::optional<std::pair<std::int64_t, double>> previous;
        std::optional<std::int64_t> zero_first, zero_last;
        for (const auto &[right, value] : discovered) {
            if (value == 0) {
                if (!zero_first)
                    zero_first = right;
                zero_last = right;
                continue;
            }
            if (!previous) {
                if (zero_first && *zero_first == begin && *zero_last - begin <= tolerance)
                    emit(begin, value > 0);
            } else if ((previous->second > 0) != (value > 0)) {
                auto lo = previous->first, hi = right;
                if (zero_first) {
                    if (*zero_last - *zero_first <= tolerance)
                        emit(*zero_first + (*zero_last - *zero_first) / 2, value > 0);
                } else {
                    const bool left_positive = previous->second > 0;
                    while (hi - lo > tolerance) {
                        const auto mid = lo + (hi - lo) / 2;
                        const auto g = f(Epoch{mid});
                        if (g == 0) {
                            lo = hi = mid;
                            break;
                        }
                        if ((g > 0) == left_positive)
                            lo = mid;
                        else
                            hi = mid;
                    }
                    emit(lo + (hi - lo) / 2, value > 0);
                }
            }
            previous = std::pair{right, value};
            zero_first.reset();
            zero_last.reset();
        }
        if (previous && zero_last && *zero_last == end && end - *zero_first <= tolerance)
            emit(end, previous->second < 0);
    }
    std::ranges::sort(result.events, [](const auto &a, const auto &b) {
        if (a.state.epoch != b.state.epoch)
            return a.state.epoch.tai_nanoseconds() < b.state.epoch.tai_nanoseconds();
        return a.kind < b.kind;
    });
    return result;
}
} // namespace orbelle::core
