#include <algorithm>
#include <cmath>
#include <limits>
#include <map>
#include <numbers>
#include <orbitforge/core/propagation/access.hpp>
#include <stdexcept>

namespace orbitforge::core {
namespace {
double norm(const std::array<double, 3> &v) {
    for (auto x : v)
        if (!std::isfinite(x))
            throw std::invalid_argument("Access geometry requires finite vectors");
    const double n = std::hypot(v[0], v[1], v[2]);
    if (!std::isfinite(n) || n == 0)
        throw std::invalid_argument("Access geometry requires nonzero finite vectors");
    return n;
}
double dot(const std::array<double, 3> &a, const std::array<double, 3> &b) {
    return a[0] * b[0] + a[1] * b[1] + a[2] * b[2];
}
} // namespace
double vector_angle(const std::array<double, 3> &a, const std::array<double, 3> &b) {
    const auto na = norm(a), nb = norm(b);
    std::array<double, 3> u{}, v{};
    for (std::size_t i = 0; i < 3; ++i) {
        u[i] = a[i] / na;
        v[i] = b[i] / nb;
    }
    return std::atan2(
        std::hypot(u[1] * v[2] - u[2] * v[1], u[2] * v[0] - u[0] * v[2], u[0] * v[1] - u[1] * v[0]),
        dot(u, v));
}
GroundObservation observe_ground_target(GroundStationLocation location,
                                        const CartesianState &target) {
    if (target.origin != OriginId::earth || target.frame != FrameId::itrf)
        throw std::invalid_argument("Ground observation requires Earth-centred ITRF state");
    const auto p = ground_station_position(location);
    (void)norm(target.position_m);
    std::array<double, 3> d{};
    for (std::size_t i = 0; i < 3; ++i) {
        if (!std::isfinite(target.velocity_mps[i]))
            throw std::invalid_argument("Ground observation requires finite velocity");
        d[i] = target.position_m[i] - p[i];
    }
    const double range = norm(d);
    const double sl = std::sin(location.longitude_rad), cl = std::cos(location.longitude_rad),
                 sp = std::sin(location.latitude_rad), cp = std::cos(location.latitude_rad);
    const double east = -sl * d[0] + cl * d[1],
                 north = -sp * cl * d[0] - sp * sl * d[1] + cp * d[2],
                 up = cp * cl * d[0] + cp * sl * d[1] + sp * d[2];
    const double horizontal = std::hypot(east, north);
    std::optional<double> azimuth;
    if (horizontal > range * 1e-14) {
        azimuth = std::atan2(east, north);
        if (*azimuth < 0)
            *azimuth += 2 * std::numbers::pi;
    }
    // Scale the WGS84 ellipsoid to a sphere, then minimize squared radius on the segment.
    constexpr double a = 6378137.0, b = a * (1 - 1 / 298.257223563);
    const std::array<double, 3> q{p[0] / a, p[1] / a, p[2] / b},
        delta{d[0] / a, d[1] / a, d[2] / b};
    const double t = std::clamp(-dot(q, delta) / dot(delta, delta), 0.0, 1.0);
    const std::array<double, 3> nearest{q[0] + t * delta[0], q[1] + t * delta[1],
                                        q[2] + t * delta[2]};
    return {azimuth, std::atan2(up, horizontal), range, dot(d, target.velocity_mps) / range,
            dot(nearest, nearest) >= 1 - 1e-14};
}
std::vector<AccessInterval> find_access_intervals(Epoch first, Epoch last, std::int64_t step,
                                                  const std::function<bool(Epoch)> &visible,
                                                  std::int64_t tolerance) {
    const auto begin = first.tai_nanoseconds(), end = last.tai_nanoseconds();
    const auto span = static_cast<std::uint64_t>(end) - static_cast<std::uint64_t>(begin);
    if (end < begin || step < 1 || tolerance < 1 || tolerance > step || !visible ||
        span / static_cast<std::uint64_t>(step) + (span % static_cast<std::uint64_t>(step) != 0) >=
            200000)
        throw std::invalid_argument(
            "Invalid Access search interval, step or tolerance (200000 points maximum)");
    std::vector<AccessInterval> result;
    bool previous = visible(first);
    Epoch start = first;
    bool clipped = previous;
    for (auto left = begin; left < end;) {
        const auto remaining = static_cast<std::uint64_t>(end) - static_cast<std::uint64_t>(left);
        const auto right = remaining <= static_cast<std::uint64_t>(step) ? end : left + step;
        const bool next = visible(Epoch{right});
        if (next != previous) {
            auto lo = left, hi = right;
            while (hi - lo > tolerance) {
                const auto mid = lo + (hi - lo) / 2;
                if (visible(Epoch{mid}) == previous)
                    lo = mid;
                else
                    hi = mid;
            }
            if (next) {
                start = Epoch{hi};
                clipped = false;
            } else if (lo > start.tai_nanoseconds())
                result.push_back({start, Epoch{lo}, clipped, false});
        }
        previous = next;
        left = right;
    }
    if (previous && end > start.tai_nanoseconds())
        result.push_back({start, last, clipped, true});
    return result;
}

namespace {
void validate_search(Epoch first, Epoch last, std::int64_t step, std::int64_t tolerance) {
    const auto a = first.tai_nanoseconds(), b = last.tai_nanoseconds();
    const auto span = static_cast<std::uint64_t>(b) - static_cast<std::uint64_t>(a);
    if (b < a || step < 1 || tolerance < 1 || tolerance > step ||
        span > static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max()) ||
        span / static_cast<std::uint64_t>(step) + (span % static_cast<std::uint64_t>(step) != 0) >=
            200000)
        throw std::invalid_argument(
            "Invalid Access interval, step or tolerance (200000 checks maximum)");
}
class ScalarSamples {
  public:
    explicit ScalarSamples(const std::function<double(Epoch)> &f) : function(f) {
        if (!function)
            throw std::invalid_argument("Access evaluator is required");
    }
    double at(std::int64_t t) {
        if (const auto it = samples.find(t); it != samples.end())
            return it->second;
        if (samples.size() >= 2'000'000)
            throw std::invalid_argument("Access exceeds 2000000 evaluations; shorten the interval");
        const auto v = function(Epoch{t});
        if (!std::isfinite(v))
            throw std::invalid_argument("Access evaluator returned a nonfinite value");
        samples.emplace(t, v);
        return v;
    }
    std::map<std::int64_t, double> samples;

  private:
    const std::function<double(Epoch)> &function;
};
// Golden-section bounded search: robust to a cusp at zenith or at the intersection
// of constraints. All candidate values come from the evaluator, not interpolation.
AccessExtremum bounded_extremum(ScalarSamples &f, std::int64_t a, std::int64_t b, bool maximum,
                                std::int64_t tolerance) {
    AccessExtremum best{Epoch{a}, f.at(a)};
    const auto improve = [&](std::int64_t t) {
        const auto v = f.at(t);
        if ((maximum ? v > best.value : v < best.value) ||
            (v == best.value && t < best.epoch.tai_nanoseconds()))
            best = {Epoch{t}, v};
        return maximum ? -v : v;
    };
    improve(b);
    if (a == b)
        return best;
    const auto mid = a + (b - a) / 2;
    improve(mid);
    // Exact plateaus have no distinguished extremum: use the earliest endpoint.
    if (f.at(a) == f.at(mid) && f.at(mid) == f.at(b))
        return best;
    constexpr double ratio = 0.3819660112501051518;
    auto left = a + static_cast<std::int64_t>(ratio * static_cast<double>(b - a));
    auto right = b - static_cast<std::int64_t>(ratio * static_cast<double>(b - a));
    auto fl = improve(left), fr = improve(right);
    while (b - a > tolerance && left > a && right < b && left < right) {
        if (fl <= fr) {
            b = right;
            right = left;
            fr = fl;
            left = a + static_cast<std::int64_t>(ratio * static_cast<double>(b - a));
            fl = improve(left);
        } else {
            a = left;
            left = right;
            fl = fr;
            right = b - static_cast<std::int64_t>(ratio * static_cast<double>(b - a));
            fr = improve(right);
        }
    }
    improve(a + (b - a) / 2);
    return best;
}
} // namespace
AccessExtremum find_access_extremum(Epoch first, Epoch last, std::int64_t step,
                                    const std::function<double(Epoch)> &value, bool maximum,
                                    std::int64_t tolerance) {
    validate_search(first, last, step, tolerance);
    ScalarSamples f(value);
    const auto end = last.tai_nanoseconds();
    auto best = AccessExtremum{first, f.at(first.tai_nanoseconds())};
    for (auto left = first.tai_nanoseconds(); left < end;) {
        const auto right = end - left <= step ? end : left + step;
        const auto next = bounded_extremum(f, left, right, maximum, tolerance);
        if (maximum ? next.value > best.value : next.value < best.value)
            best = next;
        left = right;
    }
    return best;
}
std::vector<AccessInterval> find_constraint_intervals(Epoch first, Epoch last, std::int64_t step,
                                                      const std::function<double(Epoch)> &margin,
                                                      std::int64_t tolerance) {
    validate_search(first, last, step, tolerance);
    ScalarSamples f(margin);
    const auto begin = first.tai_nanoseconds(), end = last.tai_nanoseconds();
    f.at(begin);
    // Optimizing both signs also discovers a brief hidden gap inside a visible pass.
    for (auto left = begin; left < end;) {
        const auto right = end - left <= step ? end : left + step;
        bounded_extremum(f, left, right, true, tolerance);
        bounded_extremum(f, left, right, false, tolerance);
        left = right;
    }
    std::vector<AccessInterval> result;
    auto start = begin;
    bool previous = f.at(begin) >= 0, clipped = previous;
    auto left = begin;
    // Bisection only inserts strictly inside [left, right], behind this iterator.
    // std::map preserves iterators on insertion, so newly sampled nodes need not
    // be revisited and the potentially large discovery tree need not be copied.
    for (const auto &[right, value] : f.samples) {
        if (right == begin)
            continue;
        const bool next = value >= 0;
        if (next != previous) {
            auto lo = left, hi = right;
            while (hi - lo > tolerance) {
                const auto mid = lo + (hi - lo) / 2;
                if ((f.at(mid) >= 0) == previous)
                    lo = mid;
                else
                    hi = mid;
            }
            if (next) {
                start = hi;
                clipped = false;
            } else if (lo > start)
                result.push_back({Epoch{start}, Epoch{lo}, clipped, false});
        }
        previous = next;
        left = right;
    }
    if (previous && end > start)
        result.push_back({Epoch{start}, last, clipped, true});
    return result;
}

} // namespace orbitforge::core
