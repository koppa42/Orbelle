#include "coverage_report.hpp"
#include "access_geometry.hpp"
#include <algorithm>
#include <map>

namespace orbitforge::runtime::detail {
namespace {
void check(bool ok, const char *message) {
    if (!ok)
        throw std::invalid_argument(message);
}
const application::ProjectSatellite &parent(const application::Project &p, const std::string &id) {
    auto sat_id = id;
    const auto sensor = std::ranges::find(p.sensors, id, &application::ProjectSensor::object_id);
    if (sensor != p.sensors.end())
        sat_id = sensor->parent_satellite_id;
    const auto sat = std::ranges::find_if(
        p.satellites, [&](const auto &s) { return s.orbit.object_id == sat_id; });
    check(sat != p.satellites.end(), "Coverage asset is missing or is not a satellite/sensor");
    check(sat->orbit.propagator != application::OrbitPropagator::mission,
          "Coverage requires continuous trajectories; mission sequences are not supported");
    return *sat;
}
} // namespace
void validate_coverage_assets(const application::Project &p, const application::ReportQuery &q) {
    for (const auto &id : q.object_ids)
        (void)parent(p, id);
}
CoverageReportData coverage_report(const ReportContext &context,
                                   const application::ReportDefinition &d, core::Epoch first,
                                   core::Epoch last, std::size_t total,
                                   const ReportExecution &execution,
                                   const std::function<void(std::size_t, std::size_t)> &progress) {
    check(context.time && context.frames && context.eop && context.project.earth_orientation,
          "Coverage requires confirmed UTC/EOP data and a frame provider");
    const auto cells = core::coverage_grid(d.query.coverage);
    check(first.tai_nanoseconds() < last.tai_nanoseconds(),
          "Coverage requires a positive analysis interval");
    const auto step = d.query.step_nanoseconds, tolerance = d.query.event_tolerance_nanoseconds;
    auto geometry_project = context.project;
    std::string point_id = "coverage-grid-point";
    const auto exists = [&](const std::string &id) {
        return std::ranges::any_of(geometry_project.ground_stations,
                                   [&](const auto &s) { return s.object_id == id; }) ||
               std::ranges::any_of(geometry_project.satellites,
                                   [&](const auto &s) { return s.orbit.object_id == id; }) ||
               std::ranges::any_of(geometry_project.sensors,
                                   [&](const auto &s) { return s.object_id == id; });
    };
    while (exists(point_id))
        point_id += "_";
    geometry_project.ground_stations.push_back({point_id, "Coverage grid point",
                                                cells.front().location,
                                                d.query.coverage.minimum_elevation_rad});
    struct Orbit {
        application::Session session;
        std::map<std::int64_t, core::CartesianState> cache;
    };
    std::map<std::string, Orbit> orbits;
    std::vector<AccessGeometry> assets;
    for (const auto &id : d.query.object_ids) {
        const auto &sat = parent(context.project, id);
        if (!orbits.contains(sat.orbit.object_id))
            orbits.emplace(sat.orbit.object_id,
                           Orbit{execution.session(
                                     {sat.orbit.object_id, sat.name(), "Satellite", nullptr, &sat},
                                     core::FrameId::itrf),
                                 {}});
        assets.emplace_back(geometry_project, std::vector<std::string>{point_id, id});
    }
    std::size_t state_count = 0, evaluations = 0, window_count = 0;
    constexpr std::size_t max_evaluations = 50'000'000, max_states = 500'000;
    const auto state = [&](const std::string &id,
                           core::Epoch epoch) -> const core::CartesianState & {
        execution.check_cancelled();
        auto &orbit = orbits.at(id);
        const auto key = epoch.tai_nanoseconds();
        if (const auto it = orbit.cache.find(key); it != orbit.cache.end())
            return it->second;
        check(++state_count <= max_states,
              "Coverage exceeds 500000 trajectory samples; shorten the interval or reduce assets");
        const auto batch = orbit.session.sample(std::array{epoch}, execution.check_cancelled);
        check(batch.states().size() == 1 && batch.states()[0].epoch == epoch,
              "Invalid coverage trajectory sample");
        auto s = batch.states()[0];
        if (s.frame != core::FrameId::itrf)
            s = core::transform_state(s, core::FrameId::itrf, *context.frames,
                                      context.eop->at(epoch));
        return orbit.cache.emplace(key, s).first->second;
    };
    CoverageReportData result;
    const auto duration = last.seconds_since(first);
    const auto work = cells.size() * assets.size();
    std::size_t completed = 0;
    for (const auto &cell : cells) {
        std::vector<core::AccessInterval> windows;
        for (auto &asset : assets) {
            asset.station.location = cell.location;
            const auto margin = [&](core::Epoch epoch) {
                execution.check_cancelled();
                check(++evaluations <= max_evaluations,
                      "Coverage exceeds 50 million geometry evaluations; coarsen the grid or "
                      "shorten the interval");
                const auto sample = asset.at(state(asset.satellite.orbit.object_id, epoch));
                auto value =
                    sample.observation.elevation_rad - d.query.coverage.minimum_elevation_rad;
                if (asset.sensor)
                    value =
                        std::min(value, asset.sensor->half_angle_rad - *sample.off_boresight_rad);
                if (!sample.observation.line_of_sight)
                    value = std::min(value, -1e-14);
                return value;
            };
            auto passes =
                d.query.event_search_mode == core::AccessSearchMode::fixed_grid
                    ? core::find_access_intervals(
                          first, last, step, [&](core::Epoch t) { return margin(t) >= 0; },
                          tolerance)
                    : core::find_constraint_intervals(first, last, step, margin, tolerance);
            windows.insert(windows.end(), passes.begin(), passes.end());
            check(windows.size() <= max_report_rows, "Coverage point exceeds 200000 intervals");
            ++completed;
            if (progress)
                progress(completed * total / work, total);
            execution.check_cancelled();
        }
        windows = core::merge_coverage_intervals(first, last, std::move(windows));
        window_count += windows.size();
        check(window_count <= max_report_rows,
              "Coverage exceeds 200000 stored intervals; reduce grid or duration");
        const auto stats =
            core::coverage_time_statistics(first, last, windows, d.query.coverage.include_end_gaps);
        auto &s = result.statistics;
        s.total_area_m2 += cell.area_m2;
        if (!windows.empty())
            s.ever_covered_area_m2 += cell.area_m2;
        s.mean_coverage_fraction += cell.area_m2 * stats.covered_seconds / duration;
        s.maximum_gap_seconds = std::max(s.maximum_gap_seconds, stats.maximum_gap_seconds);
        s.points.push_back({cell, std::move(windows), stats});
    }
    auto &s = result.statistics;
    s.mean_coverage_fraction /= s.total_area_m2;
    if (d.query.kind == application::ReportKind::coverage_grid) {
        for (std::size_t i = 0; i < s.points.size(); ++i) {
            const auto &p = s.points[i];
            result.rows.push_back(
                {std::to_string(i + 1), std::string("Grid ") + std::to_string(i + 1),
                 p.cell.location.longitude_rad, p.cell.location.latitude_rad, p.cell.area_m2,
                 p.statistics.covered_seconds, p.statistics.covered_seconds / duration,
                 std::int64_t(p.windows.size()), p.statistics.maximum_gap_seconds,
                 p.statistics.mean_gap_seconds});
        }
    } else {
        // Exact union windows drive both instantaneous and accumulated area; time
        // sampling does not decide whether a cell was covered between two rows.
        auto epoch = first;
        std::vector<std::size_t> cursors(s.points.size());
        for (std::size_t row = 0; row < total; ++row) {
            execution.check_cancelled();
            double area = 0, accumulated = 0;
            for (std::size_t i = 0; i < s.points.size(); ++i) {
                const auto &p = s.points[i];
                auto &cursor = cursors[i];
                while (cursor < p.windows.size() &&
                       p.windows[cursor].last.tai_nanoseconds() < epoch.tai_nanoseconds())
                    ++cursor;
                if (cursor < p.windows.size() &&
                    p.windows[cursor].first.tai_nanoseconds() <= epoch.tai_nanoseconds())
                    area += p.cell.area_m2;
                if (!p.windows.empty() &&
                    p.windows.front().first.tai_nanoseconds() <= epoch.tai_nanoseconds())
                    accumulated += p.cell.area_m2;
            }
            result.rows.push_back({std::string("coverage"), d.name, epoch, area,
                                   area / s.total_area_m2, accumulated,
                                   accumulated / s.total_area_m2});
            const auto remaining = static_cast<std::uint64_t>(last.tai_nanoseconds()) -
                                   static_cast<std::uint64_t>(epoch.tai_nanoseconds());
            epoch = remaining <= static_cast<std::uint64_t>(step)
                        ? last
                        : core::Epoch{epoch.tai_nanoseconds() + step};
        }
    }
    return result;
}
} // namespace orbitforge::runtime::detail
