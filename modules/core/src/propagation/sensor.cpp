#include <algorithm>
#include <cmath>
#include <limits>
#include <locale>
#include <numbers>
#include <numeric>
#include <orbelle/core/propagation/sensor.hpp>
#include <sstream>
#include <stdexcept>

namespace orbelle::core {
namespace {
constexpr double pi = std::numbers::pi, tau = 2 * pi;
using V = SensorVector;
using P = std::array<double, 2>;
void check(bool ok, const char *message) {
    if (!ok)
        throw std::invalid_argument(message);
}
double dot(const V &a, const V &b) { return a[0] * b[0] + a[1] * b[1] + a[2] * b[2]; }
V cross(const V &a, const V &b) {
    return {a[1] * b[2] - a[2] * b[1], a[2] * b[0] - a[0] * b[2], a[0] * b[1] - a[1] * b[0]};
}
V unit(V a) {
    const auto n = std::hypot(a[0], a[1], a[2]);
    check(std::isfinite(n) && n > 1e-30, "Sensor direction must be finite and nonzero");
    for (auto &v : a)
        v /= n;
    return a;
}
V ray(double cone, double clock) {
    return {std::sin(cone) * std::cos(clock), std::sin(cone) * std::sin(clock), std::cos(cone)};
}
double angle(const V &a, const V &b) {
    const auto c = cross(a, b);
    return std::atan2(std::hypot(c[0], c[1], c[2]), dot(a, b));
}
double turn(P a, P b, P c) { return (b[0] - a[0]) * (c[1] - a[1]) - (b[1] - a[1]) * (c[0] - a[0]); }
bool between(P a, P b, P p) {
    return p[0] >= std::min(a[0], b[0]) - 1e-12 && p[0] <= std::max(a[0], b[0]) + 1e-12 &&
           p[1] >= std::min(a[1], b[1]) - 1e-12 && p[1] <= std::max(a[1], b[1]) + 1e-12;
}
bool intersects(P a, P b, P c, P d) {
    const auto x = turn(a, b, c), y = turn(a, b, d), z = turn(c, d, a), w = turn(c, d, b);
    return (x * y < 0 && z * w < 0) || (std::abs(x) < 1e-12 && between(a, b, c)) ||
           (std::abs(y) < 1e-12 && between(a, b, d)) || (std::abs(z) < 1e-12 && between(c, d, a)) ||
           (std::abs(w) < 1e-12 && between(c, d, b));
}
double clock_span(const SensorPattern &p) {
    auto span = p.maximum_clock_rad - p.minimum_clock_rad;
    if (span < 0)
        span += tau;
    return span;
}
unsigned steps(double span, double step) {
    return std::max(1U, static_cast<unsigned>(std::ceil(span / step)));
}
void validate_step(double step) {
    check(std::isfinite(step) && step >= pi / 720 && step <= pi / 6,
          "Sensor display resolution must be in [0.25, 30] degrees");
}
V interpolate(V a, V b, double t) {
    const auto theta = angle(a, b);
    if (theta < 1e-12)
        return a;
    const auto s = std::sin(theta), x = std::sin((1 - t) * theta) / s, y = std::sin(t * theta) / s;
    return unit({x * a[0] + y * b[0], x * a[1] + y * b[1], x * a[2] + y * b[2]});
}
} // namespace

SensorGeometry::SensorGeometry(SensorPattern pattern) : pattern_(std::move(pattern)) {
    const auto &p = pattern_;
    if (p.type == SensorPatternType::simple_conic || p.type == SensorPatternType::complex_conic) {
        check(std::isfinite(p.half_angle_rad) && p.half_angle_rad > 0 && p.half_angle_rad < pi,
              "Sensor outer half angle must be in (0, 180) degrees");
        if (p.type == SensorPatternType::complex_conic) {
            check(std::isfinite(p.inner_half_angle_rad) && p.inner_half_angle_rad >= 0 &&
                      p.inner_half_angle_rad < p.half_angle_rad,
                  "Sensor inner angle must be smaller than outer angle");
            check(std::isfinite(p.minimum_clock_rad) && std::isfinite(p.maximum_clock_rad) &&
                      p.minimum_clock_rad >= 0 && p.minimum_clock_rad < tau &&
                      p.maximum_clock_rad >= 0 && p.maximum_clock_rad <= tau && clock_span(p) > 0,
                  "Invalid sensor clock interval");
        }
        return;
    }
    if (p.type == SensorPatternType::rectangular) {
        check(std::isfinite(p.horizontal_half_angle_rad) && p.horizontal_half_angle_rad > 0 &&
                  p.horizontal_half_angle_rad < pi / 2 &&
                  std::isfinite(p.vertical_half_angle_rad) && p.vertical_half_angle_rad > 0 &&
                  p.vertical_half_angle_rad < pi / 2,
              "Rectangular sensor half angles must be in (0, 90) degrees");
        const auto x = std::tan(p.horizontal_half_angle_rad),
                   y = std::tan(p.vertical_half_angle_rad);
        vertices_ = {unit({-x, -y, 1}), unit({x, -y, 1}), unit({x, y, 1}), unit({-x, y, 1})};
    } else {
        check(p.type == SensorPatternType::custom, "Unsupported sensor pattern type");
        check(p.custom_points.size() >= 3 && p.custom_points.size() <= 256,
              "Custom sensor requires 3..256 vertices");
        for (const auto &v : p.custom_points) {
            check(std::isfinite(v.cone_rad) && v.cone_rad >= 0 && v.cone_rad < pi / 2 - 1e-6 &&
                      std::isfinite(v.clock_rad) && v.clock_rad >= 0 && v.clock_rad <= tau,
                  "Custom sensor vertices require cone [0, 90) and clock [0, 360] degrees");
            vertices_.push_back(ray(v.cone_rad, v.clock_rad));
        }
    }
    for (const auto &v : vertices_)
        plane_.push_back({v[0] / v[2], v[1] / v[2]});
    const auto n = plane_.size();
    double area = 0;
    for (std::size_t i = 0; i < n; ++i) {
        const auto j = (i + 1) % n;
        check(angle(vertices_[i], vertices_[j]) > 1e-10, "Duplicate custom sensor vertices");
        area += plane_[i][0] * plane_[j][1] - plane_[j][0] * plane_[i][1];
        for (std::size_t k = i + 1; k < n; ++k) {
            const auto l = (k + 1) % n;
            if (k == j || l == i)
                continue;
            check(!intersects(plane_[i], plane_[j], plane_[k], plane_[l]),
                  "Custom sensor boundary intersects itself");
        }
    }
    check(std::abs(area) > 1e-12, "Custom sensor has zero area");
    std::vector<std::size_t> remaining(n);
    std::iota(remaining.begin(), remaining.end(), 0);
    const auto sign = area > 0 ? 1.0 : -1.0;
    while (remaining.size() > 3) {
        bool found = false;
        for (std::size_t i = 0; i < remaining.size(); ++i) {
            const auto a = remaining[(i + remaining.size() - 1) % remaining.size()],
                       b = remaining[i], c = remaining[(i + 1) % remaining.size()];
            if (sign * turn(plane_[a], plane_[b], plane_[c]) <= 1e-14)
                continue;
            bool occupied = false;
            for (auto k : remaining)
                if (k != a && k != b && k != c &&
                    sign * turn(plane_[a], plane_[b], plane_[k]) >= -1e-14 &&
                    sign * turn(plane_[b], plane_[c], plane_[k]) >= -1e-14 &&
                    sign * turn(plane_[c], plane_[a], plane_[k]) >= -1e-14) {
                    occupied = true;
                    break;
                }
            if (occupied)
                continue;
            triangles_.push_back({a, b, c});
            remaining.erase(remaining.begin() + static_cast<std::ptrdiff_t>(i));
            found = true;
            break;
        }
        check(found, "Custom sensor boundary cannot be triangulated");
    }
    triangles_.push_back({remaining[0], remaining[1], remaining[2]});
}
double SensorGeometry::margin(const V &direction) const {
    const auto d = unit(direction);
    const auto cone = std::atan2(std::hypot(d[0], d[1]), d[2]);
    const auto &p = pattern_;
    if (p.type == SensorPatternType::simple_conic)
        return p.half_angle_rad - cone;
    if (p.type == SensorPatternType::complex_conic) {
        auto result = p.half_angle_rad - cone;
        if (p.inner_half_angle_rad > 0)
            result = std::min(result, cone - p.inner_half_angle_rad);
        if (clock_span(p) < tau - 1e-14) {
            const auto a = std::asin(std::clamp(-std::sin(p.minimum_clock_rad) * d[0] +
                                                    std::cos(p.minimum_clock_rad) * d[1],
                                                -1.0, 1.0));
            const auto b = std::asin(std::clamp(std::sin(p.maximum_clock_rad) * d[0] -
                                                    std::cos(p.maximum_clock_rad) * d[1],
                                                -1.0, 1.0));
            result = std::min(result, clock_span(p) <= pi ? std::min(a, b) : std::max(a, b));
        }
        return result;
    }
    if (p.type == SensorPatternType::rectangular) {
        const auto x = std::sin(p.horizontal_half_angle_rad) * d[2] -
                       std::cos(p.horizontal_half_angle_rad) * std::abs(d[0]);
        const auto y = std::sin(p.vertical_half_angle_rad) * d[2] -
                       std::cos(p.vertical_half_angle_rad) * std::abs(d[1]);
        return std::asin(std::clamp(std::min(x, y), -1.0, 1.0));
    }
    bool inside = false;
    const P q = d[2] > 0 ? P{d[0] / d[2], d[1] / d[2]} : P{};
    double distance = pi;
    for (std::size_t i = 0, j = vertices_.size() - 1; i < vertices_.size(); j = i++) {
        const auto &a = vertices_[j], &b = vertices_[i];
        auto edge = unit(cross(a, b));
        const auto height = dot(d, edge);
        V projected{d[0] - height * edge[0], d[1] - height * edge[1], d[2] - height * edge[2]};
        auto best = std::min(angle(d, a), angle(d, b));
        if (std::hypot(projected[0], projected[1], projected[2]) > 1e-12) {
            projected = unit(projected);
            if (dot(cross(a, projected), edge) >= 0 && dot(cross(projected, b), edge) >= 0)
                best = std::min(best, std::abs(std::asin(std::clamp(height, -1.0, 1.0))));
        }
        distance = std::min(distance, best);
        const auto &pa = plane_[j], &pb = plane_[i];
        if (d[2] > 0 && ((pa[1] > q[1]) != (pb[1] > q[1])) &&
            q[0] < (pb[0] - pa[0]) * (q[1] - pa[1]) / (pb[1] - pa[1]) + pa[0])
            inside = !inside;
    }
    return inside ? distance : -distance;
}
std::vector<std::vector<V>> SensorGeometry::boundaries(double step) const {
    validate_step(step);
    const auto &p = pattern_;
    if (!vertices_.empty()) {
        std::vector<V> loop;
        for (std::size_t i = 0; i < vertices_.size(); ++i) {
            const auto a = vertices_[i], b = vertices_[(i + 1) % vertices_.size()];
            const auto n = steps(angle(a, b), step);
            for (unsigned j = 0; j < n; ++j)
                loop.push_back(interpolate(a, b, static_cast<double>(j) / n));
        }
        loop.push_back(loop.front());
        return {std::move(loop)};
    }
    const bool complex = p.type == SensorPatternType::complex_conic;
    const auto inner = complex ? p.inner_half_angle_rad : 0;
    const auto start = complex ? p.minimum_clock_rad : 0;
    const auto span = complex ? clock_span(p) : tau;
    const auto n = std::max(8U, steps(span, step));
    std::vector<V> outer, hole;
    for (unsigned i = 0; i <= n; ++i) {
        outer.push_back(ray(p.half_angle_rad, start + span * i / n));
        if (inner > 0)
            hole.push_back(ray(inner, start + span * (n - i) / n));
    }
    if (span >= tau - 1e-14) {
        std::vector<std::vector<V>> loops{std::move(outer)};
        if (inner > 0)
            loops.push_back(std::move(hole));
        return loops;
    }
    const auto radial = steps(p.half_angle_rad - inner, step);
    for (unsigned i = 1; i <= radial; ++i)
        outer.push_back(
            ray(p.half_angle_rad - (p.half_angle_rad - inner) * i / radial, start + span));
    if (inner > 0)
        outer.insert(outer.end(), hole.begin() + 1, hole.end());
    for (unsigned i = 1; i <= radial; ++i)
        outer.push_back(ray(inner + (p.half_angle_rad - inner) * i / radial, start));
    return {std::move(outer)};
}
std::vector<SensorTriangle> SensorGeometry::triangles(double step) const {
    validate_step(step);
    std::vector<SensorTriangle> result;
    if (!vertices_.empty()) {
        for (const auto &t : triangles_)
            result.push_back({vertices_[t[0]], vertices_[t[1]], vertices_[t[2]]});
        return result;
    }
    const bool complex = pattern_.type == SensorPatternType::complex_conic;
    const auto inner = complex ? pattern_.inner_half_angle_rad : 0;
    const auto start = complex ? pattern_.minimum_clock_rad : 0;
    const auto span = complex ? clock_span(pattern_) : tau;
    const auto n = std::max(8U, steps(span, step));
    const auto rings = steps(pattern_.half_angle_rad - inner, pi / 4);
    for (unsigned r = 0; r < rings; ++r) {
        const auto a = inner + (pattern_.half_angle_rad - inner) * r / rings;
        const auto b = inner + (pattern_.half_angle_rad - inner) * (r + 1) / rings;
        for (unsigned i = 0; i < n; ++i) {
            const auto c = start + span * i / n, d = start + span * (i + 1) / n;
            if (a > 0)
                result.push_back({ray(a, c), ray(b, c), ray(a, d)});
            result.push_back({ray(a, d), ray(b, c), ray(b, d)});
        }
    }
    return result;
}
SensorVector SensorAxes::to_local(const V &d) const { return {dot(x, d), dot(y, d), dot(z, d)}; }
SensorVector SensorAxes::to_world(const V &d) const {
    return {x[0] * d[0] + y[0] * d[1] + z[0] * d[2], x[1] * d[0] + y[1] * d[1] + z[1] * d[2],
            x[2] * d[0] + y[2] * d[1] + z[2] * d[2]};
}
SensorAxes sensor_axes(const V &velocity, const V &boresight, double roll) {
    check(std::isfinite(roll), "Sensor roll must be finite");
    for (auto x : velocity)
        check(std::isfinite(x), "Sensor velocity must be finite");
    const auto z = unit(boresight);
    const auto projection = [&](V v) {
        const auto a = dot(v, z);
        for (std::size_t i = 0; i < 3; ++i)
            v[i] -= a * z[i];
        return v;
    };
    auto x = projection(velocity);
    if (std::hypot(x[0], x[1], x[2]) < 1e-8) {
        V fallback{};
        const auto i = static_cast<std::size_t>(
            std::min_element(z.begin(), z.end(),
                             [](auto a, auto b) { return std::abs(a) < std::abs(b); }) -
            z.begin());
        fallback[i] = 1;
        x = projection(fallback);
    }
    x = unit(x);
    const auto y = unit(cross(z, x));
    SensorAxes axes{x, y, z};
    for (std::size_t i = 0; i < 3; ++i) {
        axes.x[i] = std::cos(roll) * x[i] + std::sin(roll) * y[i];
        axes.y[i] = -std::sin(roll) * x[i] + std::cos(roll) * y[i];
    }
    return axes;
}
SensorPattern parse_sensor_pattern(std::string_view text) {
    check(text.size() <= 256 * 1024, "Sensor pattern file exceeds 256 KiB");
    std::istringstream input{std::string(text)};
    input.imbue(std::locale::classic());
    SensorPattern result;
    result.type = SensorPatternType::custom;
    std::string line, format;
    unsigned count = 0;
    double reference = 0;
    bool data = false, ended = false, version = false, native_edges = false;
    while (std::getline(input, line)) {
        std::istringstream row(line);
        row.imbue(std::locale::classic());
        std::string key, extra;
        if (!(row >> key) || key.starts_with('#'))
            continue;
        if (!version) {
            check(key.starts_with("stk.v.") && !(row >> extra),
                  "Missing STK pattern version header");
            version = true;
            continue;
        }
        if (key == "EndPatternData") {
            check(data && !(row >> extra), "Unexpected pattern end");
            data = false;
            ended = true;
            continue;
        }
        check(!ended, "Unexpected content after sensor pattern");
        if (data) {
            double a = 0, b = 0;
            std::istringstream pair(line);
            pair.imbue(std::locale::classic());
            check(static_cast<bool>(pair >> a >> b) && !(pair >> extra) && std::isfinite(a) &&
                      std::isfinite(b),
                  "Invalid sensor pattern point");
            check(result.custom_points.size() < 257, "Too many sensor pattern points");
            if (format == "AzElMaskData")
                result.custom_points.push_back({(90 - b) * pi / 180, a * pi / 180});
            else if (format == "PatternData") {
                check(a >= 0, "Negative sensor reference radius");
                result.custom_points.push_back({std::atan2(a, reference), b * pi / 180});
            } else
                result.custom_points.push_back({a * pi / 180, b * pi / 180});
        } else if (key == "NumberPoints") {
            check(count == 0 && static_cast<bool>(row >> count) && count >= 4 && count <= 257 &&
                      !(row >> extra),
                  "Invalid sensor point count");
        } else if (key == "ReferenceDistance") {
            check(reference == 0 && static_cast<bool>(row >> reference) &&
                      std::isfinite(reference) && reference > 0 && !(row >> extra),
                  "Invalid reference distance");
        } else if (key == "InterpolateAzElPts") {
            std::string value;
            check(
                static_cast<bool>(row >> value) && value == "No" && !(row >> extra),
                "Only native great-circle sensor boundaries (InterpolateAzElPts No) are supported");
            native_edges = true;
        } else if (key == "HalfAngleAzimuthData" || key == "AzElMaskData" || key == "PatternData") {
            check(count != 0 && !(row >> extra) && (key != "PatternData" || reference > 0),
                  "Incomplete sensor pattern header");
            format = key;
            data = true;
        } else
            check(false, "Unsupported sensor pattern directive");
    }
    check(ended && result.custom_points.size() == count, "Sensor pattern is incomplete");
    check(format == "PatternData" || native_edges,
          "Angular pattern import requires InterpolateAzElPts No; implicit STK interpolation is "
          "unsupported");
    check(angle(ray(result.custom_points.front().cone_rad, result.custom_points.front().clock_rad),
                ray(result.custom_points.back().cone_rad, result.custom_points.back().clock_rad)) <
              1e-10,
          "Sensor pattern must repeat its first point at the end");
    result.custom_points.pop_back();
    (void)SensorGeometry(result);
    return result;
}
} // namespace orbelle::core
