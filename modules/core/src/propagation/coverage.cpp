#include <algorithm>
#include <cmath>
#include <orbitforge/core/propagation/coverage.hpp>
#include <stdexcept>

namespace orbitforge::core {
namespace {
constexpr double pi = std::numbers::pi;
void check(bool value, const char *message) {
    if (!value)
        throw std::invalid_argument(message);
}
void interval(Epoch first, Epoch last) {
    check(first.tai_nanoseconds() < last.tai_nanoseconds(),
          "Coverage requires a positive analysis interval");
}
// Integral of M(phi)*N(phi)*cos(phi), per radian of longitude.
double area_primitive(double latitude) {
    constexpr double a = 6378137.0, f = 1.0 / 298.257223563;
    constexpr double e2 = f * (2 - f);
    const double e = std::sqrt(e2), s = std::sin(latitude);
    return a * a * (1 - e2) * .5 * (s / (1 - e2 * s * s) + std::atanh(e * s) / e);
}
} // namespace
void validate_coverage_grid(const CoverageGrid &g) {
    for (double v : {g.south_rad, g.north_rad, g.west_rad, g.east_rad, g.resolution_rad,
                     g.minimum_elevation_rad})
        check(std::isfinite(v), "Coverage grid parameters must be finite");
    check(g.south_rad >= -pi / 2 && g.north_rad <= pi / 2 && g.south_rad < g.north_rad,
          "Coverage latitude bounds must increase within [-90,90] degrees");
    check(g.west_rad >= -pi && g.west_rad <= pi && g.east_rad >= -pi && g.east_rad <= pi &&
              g.west_rad != g.east_rad && !(g.west_rad == pi && g.east_rad == -pi),
          "Coverage longitude bounds must define a nonempty eastward region");
    check(g.resolution_rad > 0 && g.resolution_rad <= pi,
          "Coverage resolution must be in (0,180] degrees");
    check(g.minimum_elevation_rad >= 0 && g.minimum_elevation_rad <= pi / 2,
          "Coverage minimum elevation must be in [0,90] degrees");
}
double coverage_longitude_span(const CoverageGrid &g) {
    validate_coverage_grid(g);
    const auto span = g.east_rad - g.west_rad;
    return span > 0 ? span : span + 2 * pi;
}
std::vector<CoverageCell> coverage_grid(const CoverageGrid &g) {
    const auto longitude_span = coverage_longitude_span(g);
    const double latitude_span = g.north_rad - g.south_rad;
    const auto count = std::max(1.0, std::ceil(latitude_span / g.resolution_rad));
    check(std::isfinite(count) && count <= 4096, "Coverage grid exceeds 4096 cells");
    const auto bands = static_cast<std::size_t>(count);
    std::vector<CoverageCell> cells;
    for (std::size_t row = 0; row < bands; ++row) {
        const auto south = g.south_rad + latitude_span * (double(row) / double(bands));
        const auto north = row + 1 == bands
                               ? g.north_rad
                               : g.south_rad + latitude_span * (double(row + 1) / double(bands));
        const auto latitude = (south + north) * .5;
        const auto columns_d =
            std::max(1.0, std::ceil(longitude_span * std::cos(latitude) / g.resolution_rad));
        check(std::isfinite(columns_d) && columns_d <= double(4096 - cells.size()),
              "Coverage grid exceeds 4096 cells; increase resolution");
        const auto columns = static_cast<std::size_t>(columns_d);
        const auto strip_area = area_primitive(north) - area_primitive(south);
        for (std::size_t col = 0; col < columns; ++col) {
            const auto west = g.west_rad + longitude_span * (double(col) / double(columns));
            const auto east = g.west_rad + longitude_span * (double(col + 1) / double(columns));
            const auto longitude = std::remainder((west + east) * .5, 2 * pi);
            const auto area = strip_area * (east - west);
            check(area > 0 && std::isfinite(area), "Coverage cell area is not representable");
            cells.push_back({{longitude, latitude, 0}, south, north, west, east, area});
        }
    }
    return cells;
}
std::vector<AccessInterval> merge_coverage_intervals(Epoch first, Epoch last,
                                                     std::vector<AccessInterval> windows) {
    interval(first, last);
    for (const auto &w : windows)
        check(w.first.tai_nanoseconds() >= first.tai_nanoseconds() &&
                  w.last.tai_nanoseconds() <= last.tai_nanoseconds() &&
                  w.first.tai_nanoseconds() < w.last.tai_nanoseconds(),
              "Coverage windows must have positive duration within the analysis interval");
    std::ranges::sort(windows, {}, [](const auto &w) { return w.first.tai_nanoseconds(); });
    std::vector<AccessInterval> merged;
    for (auto w : windows) {
        if (merged.empty() || w.first.tai_nanoseconds() > merged.back().last.tai_nanoseconds()) {
            merged.push_back(w);
        } else if (w.last.tai_nanoseconds() > merged.back().last.tai_nanoseconds()) {
            merged.back().last = w.last;
        }
    }
    for (auto &w : merged) {
        w.start_clipped = w.first == first;
        w.stop_clipped = w.last == last;
    }
    return merged;
}
CoverageTimeStatistics coverage_time_statistics(Epoch first, Epoch last,
                                                const std::vector<AccessInterval> &windows,
                                                bool include_ends) {
    const auto merged = merge_coverage_intervals(first, last, windows);
    CoverageTimeStatistics stats;
    double gaps = 0;
    const auto gap = [&](double duration) {
        if (duration <= 0)
            return;
        ++stats.gap_count;
        gaps += duration;
        stats.maximum_gap_seconds = std::max(stats.maximum_gap_seconds, duration);
    };
    auto previous = first;
    for (std::size_t i = 0; i < merged.size(); ++i) {
        const auto &w = merged[i];
        stats.covered_seconds += w.last.seconds_since(w.first);
        if (include_ends || i > 0)
            gap(w.first.seconds_since(previous));
        previous = w.last;
    }
    if (include_ends || merged.empty())
        gap(last.seconds_since(previous));
    if (stats.gap_count)
        stats.mean_gap_seconds = gaps / double(stats.gap_count);
    return stats;
}
} // namespace orbitforge::core
