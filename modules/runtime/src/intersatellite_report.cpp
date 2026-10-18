#include "intersatellite_report.hpp"
#include <algorithm>
#include <map>
#include <orbitforge/core/propagation/relative_motion.hpp>
namespace orbitforge::runtime::detail {
namespace {
void check(bool ok, const char *message) {
    if (!ok)
        throw std::invalid_argument(message);
}
const application::ProjectSatellite &satellite(const application::Project &p,
                                               const std::string &id) {
    const auto it = std::ranges::find_if(
        p.satellites, [&](const auto &sat) { return sat.orbit.object_id == id; });
    check(it != p.satellites.end(), "Intersatellite reports require two satellite IDs");
    check(it->orbit.propagator != application::OrbitPropagator::mission,
          "Intersatellite reports require continuous trajectories; mission sequences are not "
          "supported");
    return *it;
}
} // namespace
IntersatelliteReportData
intersatellite_report(const ReportContext &context, const application::ReportDefinition &d,
                      core::Epoch first, core::Epoch last, std::size_t total,
                      const ReportExecution &execution,
                      const std::function<void(std::size_t, std::size_t)> &progress) {
    check(context.time && context.frames && context.eop && context.project.earth_orientation,
          "Intersatellite visibility requires confirmed UTC and EOP data and a frame provider");
    const auto &primary = satellite(context.project, d.query.object_ids[0]);
    const auto &target = satellite(context.project, d.query.object_ids[1]);
    const auto a = execution.session(
        {primary.orbit.object_id, primary.display_name, "Satellite", nullptr, &primary},
        core::FrameId::gcrf);
    const auto b = execution.session(
        {target.orbit.object_id, target.display_name, "Satellite", nullptr, &target},
        core::FrameId::gcrf);
    const auto name =
        (primary.display_name.empty() ? primary.orbit.object_id : primary.display_name) + " → " +
        (target.display_name.empty() ? target.orbit.object_id : target.display_name);
    struct Sample {
        core::RelativeMotion motion;
        double clearance;
    };
    std::map<std::int64_t, Sample> cache;
    std::size_t completed = 0;
    const auto at = [&](core::Epoch epoch) -> const Sample & {
        execution.check_cancelled();
        const auto key = epoch.tai_nanoseconds();
        if (const auto i = cache.find(key); i != cache.end())
            return i->second;
        check(cache.size() < 2'000'000,
              "Intersatellite analysis exceeds two million evaluations; shorten the interval");
        const auto visited =
            static_cast<std::size_t>((static_cast<std::uint64_t>(key) -
                                      static_cast<std::uint64_t>(first.tai_nanoseconds())) /
                                     static_cast<std::uint64_t>(d.query.step_nanoseconds));
        completed = std::max(completed, std::min(visited, total - 1));
        if (progress)
            progress(completed, total);
        execution.check_cancelled();
        const auto eop = context.eop->at(epoch);
        const auto state = [&](const application::Session &session) {
            auto s = session.sample(std::array{epoch}, execution.check_cancelled).states().front();
            check(s.epoch == epoch, "Intersatellite provider returned a different epoch");
            if (s.frame != core::FrameId::gcrf)
                s = core::transform_state(s, core::FrameId::gcrf, *context.frames, eop);
            return s;
        };
        const auto sa = state(a), sb = state(b);
        auto motion = core::relative_motion(sa, sb);
        const auto fixed = context.frames->gcrf_to_itrf_state(epoch, eop);
        const auto clearance = core::intersatellite_clearance(fixed.apply(sa), fixed.apply(sb));
        return cache.emplace(key, Sample{motion, clearance}).first->second;
    };
    const auto step = d.query.step_nanoseconds, tolerance = d.query.event_tolerance_nanoseconds;
    auto windows =
        d.query.event_search_mode == core::AccessSearchMode::fixed_grid
            ? core::find_access_intervals(
                  first, last, step, [&](core::Epoch t) { return at(t).clearance >= 0; }, tolerance)
            : core::find_constraint_intervals(
                  first, last, step, [&](core::Epoch t) { return at(t).clearance; }, tolerance);
    const auto range = [&](core::Epoch t) { return at(t).motion.range_m; };
    const auto minimum = core::find_access_extremum(first, last, step, range, false, tolerance);
    const auto maximum = core::find_access_extremum(first, last, step, range, true, tolerance);
    std::vector<results::AccessWindow> passes;
    for (const auto &w : windows)
        passes.emplace_back(w.first, w.last, w.start_clipped, w.stop_clipped);
    const results::AccessQuery query{first, last, std::chrono::nanoseconds{step},
                                     std::chrono::nanoseconds{tolerance},
                                     d.query.event_search_mode};
    const auto visibility = results::AccessWindows{std::move(passes), query, {}}.statistics();
    IntersatelliteReportData result{{}, {minimum, maximum, visibility, std::move(windows)}};
    if (d.query.kind == application::ReportKind::intersatellite_access) {
        check(result.statistics.windows.size() <= max_report_rows,
              "Intersatellite access exceeds 200000 rows");
        for (const auto &w : result.statistics.windows) {
            const auto closest =
                core::find_access_extremum(w.first, w.last, step, range, false, tolerance);
            result.rows.push_back({target.orbit.object_id, name, w.first, w.last,
                                   w.last.seconds_since(w.first), std::int64_t(w.start_clipped),
                                   std::int64_t(w.stop_clipped), closest.value, closest.epoch});
        }
    } else {
        auto epoch = first;
        for (std::size_t i = 0; i < total; ++i) {
            const auto &s = at(epoch);
            const auto &m = s.motion;
            result.rows.push_back(
                {target.orbit.object_id, name, epoch, m.range_m,
                 m.range_rate_mps ? results::Cell{*m.range_rate_mps} : results::Cell{},
                 m.relative_speed_mps, m.ric_position_m[0], m.ric_position_m[1],
                 m.ric_position_m[2], std::int64_t(s.clearance >= 0)});
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
