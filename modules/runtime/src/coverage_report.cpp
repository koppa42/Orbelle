#include "coverage_report.hpp"
#include "access_geometry.hpp"
#include <algorithm>
#include <cmath>
#include <limits>
#include <map>
#include <numeric>
#include <unordered_map>

namespace orbelle::runtime::detail {
namespace {
void check(bool ok, const char *message) {
    if (!ok)
        throw std::invalid_argument(message);
}
// Coverage points lie on the WGS84 surface. Its outward normal is a separating
// plane for the convex ellipsoid, so nonnegative elevation also ensures LOS.
// Cache the fixed observer coordinates; azimuth/range/range-rate are not outputs
// of the coverage constraint evaluator.
struct Observer {
    std::array<double, 3> position;
    double sp, cp, sl, cl;
    explicit Observer(core::GroundStationLocation p)
        : position(core::ground_station_position(p)), sp(std::sin(p.latitude_rad)),
          cp(std::cos(p.latitude_rad)), sl(std::sin(p.longitude_rad)),
          cl(std::cos(p.longitude_rad)) {}
    double margin(const AccessGeometry &asset, const core::CartesianState &state,
                  double minimum_elevation, const std::array<double, 3> &target) const {
        std::array<double, 3> d{}, axis{};
        for (std::size_t i = 0; i < 3; ++i) {
            d[i] = state.position_m[i] - position[i];
            axis[i] = target[i] - state.position_m[i];
        }
        check(d[0] != 0 || d[1] != 0 || d[2] != 0,
              "Coverage target coincides with the ground observer");
        const auto east = -sl * d[0] + cl * d[1];
        const auto north = -sp * cl * d[0] - sp * sl * d[1] + cp * d[2];
        const auto up = cp * cl * d[0] + cp * sl * d[1] + sp * d[2];
        auto value = std::atan2(up, std::hypot(east, north)) - minimum_elevation;
        if (asset.sensor) {
            const auto axes = core::sensor_axes(state.velocity_mps, axis, asset.sensor->tilt_x_rad,
                                                asset.sensor->tilt_y_rad, asset.sensor->roll_rad);
            for (auto &v : d)
                v = -v;
            auto sensor_margin = asset.pattern->margin(axes.to_local(d));
            if (std::abs(sensor_margin) <= 1e-14)
                sensor_margin = 0;
            value = std::min(value, sensor_margin);
        }
        return value;
    }
};
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
// Search only gaps in the existing union, expanding out to the ORIGINAL search
// grid. This keeps every potentially relevant extremum/bisection bracket intact;
// restarting the grid at a gap boundary would change short-event discovery.
std::vector<std::pair<core::Epoch, core::Epoch>>
search_spans(core::Epoch first, core::Epoch last, std::int64_t step,
             const std::vector<core::AccessInterval> &windows) {
    const auto span = static_cast<std::uint64_t>(last.tai_nanoseconds()) -
                      static_cast<std::uint64_t>(first.tai_nanoseconds());
    check(span <= static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max()),
          "Coverage search interval is too large");
    const auto stride = static_cast<std::uint64_t>(step);
    std::vector<std::pair<core::Epoch, core::Epoch>> spans;
    const auto add = [&](core::Epoch begin, core::Epoch end) {
        if (begin == end)
            return;
        auto a = static_cast<std::uint64_t>(begin.tai_nanoseconds()) -
                 static_cast<std::uint64_t>(first.tai_nanoseconds());
        auto b = static_cast<std::uint64_t>(end.tai_nanoseconds()) -
                 static_cast<std::uint64_t>(first.tai_nanoseconds());
        a -= a % stride;
        if (b % stride)
            b += std::min(span - b, stride - b % stride);
        const core::Epoch left{first.tai_nanoseconds() + static_cast<std::int64_t>(a)};
        const core::Epoch right{first.tai_nanoseconds() + static_cast<std::int64_t>(b)};
        if (!spans.empty() && left.tai_nanoseconds() <= spans.back().second.tai_nanoseconds())
            spans.back().second = right;
        else
            spans.emplace_back(left, right);
    };
    auto previous = first;
    for (const auto &w : windows) {
        add(previous, w.first);
        previous = w.last;
    }
    add(previous, last);
    return spans;
}
} // namespace
void validate_coverage_assets(const application::Project &p, const application::ReportQuery &q) {
    for (const auto &id : q.object_ids)
        (void)parent(p, id);
}
CoverageReportData coverage_report(const ReportContext &context,
                                   const application::ReportDefinition &d, core::Epoch first,
                                   core::Epoch last, const CoverageReportPlan &plan,
                                   const ReportExecution &execution,
                                   const std::function<void(std::size_t, std::size_t)> &progress) {
    check(context.time && context.frames && context.eop && context.project.earth_orientation,
          "Coverage requires confirmed UTC/EOP data and a frame provider");
    const auto total = plan.rows;
    check(first.tai_nanoseconds() < last.tai_nanoseconds(),
          "Coverage requires a positive analysis interval");
    const auto step = d.query.step_nanoseconds, tolerance = d.query.event_tolerance_nanoseconds;
    auto geometry_project = context.project;
    std::string point_id = "coverage-grid-point";
    const auto exists = [&](const std::string &id) {
        return std::ranges::any_of(geometry_project.regions,
                                   [&](const auto &r) { return r.object_id == id; }) ||
               std::ranges::any_of(geometry_project.ground_stations,
                                   [&](const auto &s) { return s.object_id == id; }) ||
               std::ranges::any_of(geometry_project.satellites,
                                   [&](const auto &s) { return s.orbit.object_id == id; }) ||
               std::ranges::any_of(geometry_project.sensors,
                                   [&](const auto &s) { return s.object_id == id; });
    };
    while (exists(point_id))
        point_id += "_";
    geometry_project.ground_stations.push_back(
        {point_id, "Coverage grid point", {0, 0, 0}, d.query.coverage.minimum_elevation_rad});
    struct Orbit {
        application::Session session;
        // Exact epoch lookup only; search order is owned by the Access algorithm.
        std::unordered_map<std::int64_t, core::CartesianState> cache;
        std::vector<AccessGeometry> assets;
    };
    std::vector<Orbit> orbits;
    std::map<std::string, std::size_t> orbit_index;
    for (const auto &id : d.query.object_ids) {
        const auto &sat = parent(context.project, id);
        if (!orbit_index.contains(sat.orbit.object_id)) {
            orbit_index.emplace(sat.orbit.object_id, orbits.size());
            orbits.push_back(
                {execution.session({sat.orbit.object_id, sat.name(), "Satellite", nullptr, &sat},
                                   core::FrameId::itrf),
                 {},
                 {}});
        }
        orbits[orbit_index.at(sat.orbit.object_id)].assets.emplace_back(
            geometry_project, std::vector<std::string>{point_id, id});
    }
    std::size_t window_count = 0;
    const auto window_capacity =
        (context.coverage_memory_bytes - plan.storage_bytes) / coverage_window_storage_bytes;
    constexpr std::size_t state_cache_capacity = 65536, frame_cache_capacity = 32768;
    // Providers are captured immutable inputs. Share exact-epoch frame transforms
    // across spacecraft within this report, including the rotation-rate term.
    std::unordered_map<std::int64_t, core::StateTransform> gcrf_frames, teme_frames;
    const auto state = [&](Orbit &orbit, core::Epoch epoch) -> const core::CartesianState & {
        execution.check_cancelled();
        const auto key = epoch.tai_nanoseconds();
        if (const auto it = orbit.cache.find(key); it != orbit.cache.end())
            return it->second;
        const auto batch = orbit.session.sample(std::array{epoch}, execution.check_cancelled);
        check(batch.states().size() == 1 && batch.states()[0].epoch == epoch,
              "Invalid coverage trajectory sample");
        auto s = batch.states()[0];
        if (s.frame == core::FrameId::gcrf || s.frame == core::FrameId::teme) {
            auto &cache = s.frame == core::FrameId::gcrf ? gcrf_frames : teme_frames;
            auto it = cache.find(key);
            if (it == cache.end()) {
                if (cache.size() >= frame_cache_capacity)
                    cache.clear();
                const auto eop = context.eop->at(epoch);
                check(eop.sample.epoch == epoch, "Coverage EOP epoch mismatch");
                auto transform = s.frame == core::FrameId::gcrf
                                     ? context.frames->gcrf_to_itrf_state(epoch, eop)
                                     : context.frames->teme_to_itrf_state(epoch, eop);
                it = cache.emplace(key, std::move(transform)).first;
            }
            s = it->second.apply(s);
        } else if (s.frame != core::FrameId::itrf)
            s = core::transform_state(s, core::FrameId::itrf, *context.frames,
                                      context.eop->at(epoch));
        check(s.origin == core::OriginId::earth && s.frame == core::FrameId::itrf,
              "Coverage requires Earth-centred ITRF states");
        for (std::size_t i = 0; i < 3; ++i)
            check(std::isfinite(s.position_m[i]) && std::isfinite(s.velocity_mps[i]),
                  "Coverage trajectory state must be finite");
        if (orbit.cache.size() >= state_cache_capacity)
            orbit.cache.clear();
        return orbit.cache.emplace(key, s).first->second;
    };
    CoverageReportData result;
    result.statistics.grid = application::report_coverage_bounds(context.project, d.query);
    if (!d.query.region_id.empty())
        result.statistics.region = application::report_region(context.project, d.query).boundary;
    const auto duration = last.seconds_since(first);
    result.statistics.points.reserve(plan.points);
    result.rows.reserve(total);
    const auto span_ticks = static_cast<std::uint64_t>(last.tai_nanoseconds()) -
                            static_cast<std::uint64_t>(first.tai_nanoseconds());
    const auto chunk_ticks = 64 * static_cast<std::uint64_t>(step);
    const auto chunk_count = (span_ticks - 1) / chunk_ticks + 1;
    const auto work =
        static_cast<long double>(plan.points) * d.query.object_ids.size() * chunk_count;
    long double completed = 0;
    std::size_t last_progress = 0;
    const auto notify = [&] {
        const auto current = std::min(total, static_cast<std::size_t>(completed / work * total));
        if (progress && current != last_progress) {
            progress(current, total);
            last_progress = current;
        }
    };
    // Check every selected trajectory's endpoints, even if other assets later
    // provide full coverage. Invalid inputs must not disappear behind pruning.
    for (auto &orbit : orbits) {
        (void)state(orbit, first);
        (void)state(orbit, last);
    }
    std::vector<std::size_t> order(orbits.size());
    std::iota(order.begin(), order.end(), 0);
    if (orbits.size() > 32) {
        // Constellation inputs often group nearby phases/planes. Visit diverse
        // initial position/velocity directions first so the union closes gaps
        // early. This only prioritizes work; it never excludes an asset.
        std::vector<std::array<double, 6>> directions;
        for (auto &orbit : orbits) {
            const auto &s = state(orbit, first);
            const auto r = std::hypot(s.position_m[0], s.position_m[1], s.position_m[2]);
            const auto v = std::hypot(s.velocity_mps[0], s.velocity_mps[1], s.velocity_mps[2]);
            std::array<double, 6> direction{};
            for (std::size_t axis = 0; axis < 3; ++axis) {
                direction[axis] = r > 0 ? s.position_m[axis] / r : 0;
                direction[axis + 3] = v > 0 ? s.velocity_mps[axis] / v : 0;
            }
            directions.push_back(direction);
        }
        std::vector<double> distances(orbits.size(), std::numeric_limits<double>::max());
        std::size_t current = 0;
        for (auto &index : order) {
            execution.check_cancelled();
            index = current;
            distances[current] = -1;
            double best = -1;
            std::size_t next = current;
            for (std::size_t i = 0; i < directions.size(); ++i) {
                if (distances[i] < 0)
                    continue;
                double distance = 0;
                for (std::size_t axis = 0; axis < 6; ++axis) {
                    const auto delta = directions[i][axis] - directions[current][axis];
                    distance += delta * delta;
                }
                distances[i] = std::min(distances[i], distance);
                if (distances[i] > best) {
                    best = distances[i];
                    next = i;
                }
            }
            current = next;
        }
    }
    const auto process = [&](const std::vector<core::CoverageCell> &cells) {
        execution.check_cancelled();
        std::vector<Observer> observers;
        for (const auto &cell : cells)
            observers.emplace_back(cell.location);
        std::vector<std::vector<core::AccessInterval>> point_windows(cells.size());
        const auto block_start = completed;
        std::uint64_t finished_chunks = 0;
        auto chunk_begin = first;
        // Visit all points/assets within the same time window before advancing.
        // A point-by-point multi-day sweep would evict common sample epochs before
        // the next point can reuse them, despite the bounded state/frame caches.
        while (chunk_begin != last) {
            execution.check_cancelled();
            const auto remaining_ticks = static_cast<std::uint64_t>(last.tai_nanoseconds()) -
                                         static_cast<std::uint64_t>(chunk_begin.tai_nanoseconds());
            const core::Epoch chunk_end{
                chunk_begin.tai_nanoseconds() +
                static_cast<std::int64_t>(std::min(chunk_ticks, remaining_ticks))};
            std::vector<std::vector<core::AccessInterval>> chunk_windows(cells.size());
            std::vector<std::size_t> active(cells.size());
            std::iota(active.begin(), active.end(), 0);
            for (const auto orbit_id : order) {
                if (active.empty())
                    break;
                auto &orbit = orbits[orbit_id];
                for (auto &asset : orbit.assets) {
                    if (active.empty())
                        break;
                    const auto target =
                        asset.pointing_target
                            ? core::ground_station_position(asset.pointing_target->location)
                            : std::array<double, 3>{};
                    std::vector<std::size_t> remaining;
                    remaining.reserve(active.size());
                    completed += cells.size() - active.size();
                    for (const auto index : active) {
                        auto &windows = chunk_windows[index];
                        const auto margin = [&](core::Epoch epoch) {
                            execution.check_cancelled();
                            return observers[index].margin(asset, state(orbit, epoch),
                                                           d.query.coverage.minimum_elevation_rad,
                                                           target);
                        };
                        std::vector<core::AccessInterval> passes;
                        for (const auto &[begin, end] :
                             search_spans(chunk_begin, chunk_end, step, windows)) {
                            auto found =
                                d.query.event_search_mode == core::AccessSearchMode::fixed_grid
                                    ? core::find_access_intervals(
                                          begin, end, step,
                                          [&](core::Epoch t) { return margin(t) >= 0; }, tolerance)
                                    : core::find_constraint_intervals(begin, end, step, margin,
                                                                      tolerance);
                            for (const auto &w : found) {
                                if (!passes.empty() && w.first.tai_nanoseconds() <=
                                                           passes.back().last.tai_nanoseconds()) {
                                    if (w.last.tai_nanoseconds() >
                                        passes.back().last.tai_nanoseconds())
                                        passes.back().last = w.last;
                                } else {
                                    check(passes.size() < window_capacity - window_count,
                                          "Coverage intervals exceed the memory budget; increase "
                                          "the coverage memory budget or reduce result size");
                                    passes.push_back(w);
                                }
                            }
                        }
                        windows.insert(windows.end(), passes.begin(), passes.end());
                        window_count -= windows.size() - passes.size();
                        windows = core::merge_coverage_intervals(chunk_begin, chunk_end,
                                                                 std::move(windows));
                        window_count += windows.size();
                        if (windows.size() != 1 || windows.front().first != chunk_begin ||
                            windows.front().last != chunk_end)
                            remaining.push_back(index);
                        ++completed;
                        notify();
                        execution.check_cancelled();
                    }
                    active = std::move(remaining);
                }
                // Only one parent orbit's sampled states need remain resident. Sensors
                // sharing that parent reuse its cache before it is released.
                if (orbits.size() > 1) {
                    orbit.cache.clear();
                    orbit.cache.rehash(0);
                }
            }
            for (std::size_t index = 0; index < cells.size(); ++index) {
                execution.check_cancelled();
                auto &windows = point_windows[index];
                const auto &local = chunk_windows[index];
                window_count -= windows.size() + local.size();
                for (const auto &window : local) {
                    if (!windows.empty() && windows.back().last == window.first)
                        windows.back().last = window.last;
                    else
                        windows.push_back(window);
                }
                window_count += windows.size();
            }
            chunk_begin = chunk_end;
            ++finished_chunks;
            // Include work pruned after a time window is fully covered.
            completed = block_start + static_cast<long double>(cells.size()) *
                                          d.query.object_ids.size() * finished_chunks;
            notify();
        }
        for (std::size_t index = 0; index < cells.size(); ++index) {
            const auto &cell = cells[index];
            auto &windows = point_windows[index];
            windows = core::merge_coverage_intervals(first, last, std::move(windows));
            const auto stats = core::coverage_time_statistics(first, last, windows,
                                                              d.query.coverage.include_end_gaps);
            auto &s = result.statistics;
            s.total_area_m2 += cell.area_m2;
            if (!windows.empty())
                s.ever_covered_area_m2 += cell.area_m2;
            s.mean_coverage_fraction += cell.area_m2 * stats.covered_seconds / duration;
            s.maximum_gap_seconds = std::max(s.maximum_gap_seconds, stats.maximum_gap_seconds);
            s.points.push_back({cell, std::move(windows), stats});
        }
    };
    // Only retained points enter a working block. The result remains complete;
    // a dense bounding rectangle or polygon exterior is never materialized.
    std::vector<core::CoverageCell> block;
    block.reserve(1024);
    application::visit_report_coverage_grid(
        context.project, d.query,
        [&](const auto &cell) {
            block.push_back(cell);
            if (block.size() == 1024) {
                process(block);
                block.clear();
            }
        },
        execution.check_cancelled);
    if (!block.empty())
        process(block);
    completed = work;
    notify();
    auto &s = result.statistics;
    check(s.points.size() == plan.points, "Coverage point count changed during calculation");
    s.mean_coverage_fraction /= s.total_area_m2;
    if (d.query.kind == application::ReportKind::coverage_grid) {
        for (std::size_t i = 0; i < s.points.size(); ++i) {
            if (i % 256 == 0)
                execution.check_cancelled();
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
        // Range additions avoid scanning every ground point at every output
        // time. The final partial time step retains the closed-end convention.
        std::vector<double> instant_changes(total), accumulated_changes(total);
        const auto stride = static_cast<std::uint64_t>(step);
        const auto offset = [&](core::Epoch epoch) {
            return static_cast<std::uint64_t>(epoch.tai_nanoseconds()) -
                   static_cast<std::uint64_t>(first.tai_nanoseconds());
        };
        for (const auto &p : s.points) {
            execution.check_cancelled();
            if (!p.windows.empty()) {
                const auto ticks = offset(p.windows.front().first);
                accumulated_changes[ticks / stride + (ticks % stride != 0)] += p.cell.area_m2;
            }
            for (const auto &w : p.windows) {
                const auto a = offset(w.first), b = offset(w.last);
                const auto begin = a / stride + (a % stride != 0);
                const auto end = w.last == last ? total - 1 : b / stride;
                if (begin <= end) {
                    instant_changes[begin] += p.cell.area_m2;
                    if (end + 1 < total)
                        instant_changes[end + 1] -= p.cell.area_m2;
                }
            }
        }
        auto epoch = first;
        double area = 0, accumulated = 0;
        for (std::size_t row = 0; row < total; ++row) {
            execution.check_cancelled();
            area += instant_changes[row];
            accumulated += accumulated_changes[row];
            const auto visible = std::clamp(area, 0., s.total_area_m2);
            const auto ever = std::clamp(accumulated, 0., s.total_area_m2);
            result.rows.push_back({std::string("coverage"), d.name, epoch, visible,
                                   visible / s.total_area_m2, ever, ever / s.total_area_m2});
            const auto remaining = static_cast<std::uint64_t>(last.tai_nanoseconds()) -
                                   static_cast<std::uint64_t>(epoch.tai_nanoseconds());
            epoch = remaining <= static_cast<std::uint64_t>(step)
                        ? last
                        : core::Epoch{epoch.tai_nanoseconds() + step};
        }
    }
    return result;
}
} // namespace orbelle::runtime::detail
