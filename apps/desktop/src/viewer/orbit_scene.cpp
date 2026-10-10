#include "orbit_scene.hpp"
#include <algorithm>
#include <chrono>
#include <cmath>
#include <limits>
#include <orbitforge/runtime/j2.hpp>
#include <orbitforge/runtime/project.hpp>

namespace orbitforge::desktop {
application::Session make_orbit_session(const OrbitInput &input, std::uint64_t revision) {
    return runtime::make_orbit_session(input, revision);
}
std::shared_ptr<const OrbitScene>
prepare_orbit_scene(OrbitInput input, application::Session session,
                    const core::IFrameProvider &frames, const core::IEarthOrientationProvider &eop,
                    const std::atomic_bool &cancelled,
                    std::optional<std::pair<core::Epoch, core::Epoch>> cache_interval) {
    const auto started = std::chrono::steady_clock::now();
    const auto check_cancel = [&] {
        if (cancelled.load())
            throw OrbitCancelled{};
    };
    check_cancel();
    const auto &info = eop.data_info();
    const auto analysis_first = input.first.tai_nanoseconds(),
               analysis_last = input.last.tai_nanoseconds();
    const auto first = cache_interval ? cache_interval->first.tai_nanoseconds() : analysis_first;
    const auto last = cache_interval ? cache_interval->second.tai_nanoseconds() : analysis_last;
    if (last <= first)
        throw std::invalid_argument("Analysis end must be later than its start");
    if (!core::eop_covers(info, input.first) || !core::eop_covers(info, input.last))
        throw std::out_of_range("Analysis interval is outside the installed EOP coverage");
    if (first < analysis_first || last > analysis_last)
        throw std::out_of_range("Display cache must lie inside the analysis interval");
    if (input.step_seconds < 1 || input.step_seconds > 60)
        throw std::invalid_argument("Display step must be between 1 and 60 seconds");
    const auto step = static_cast<std::int64_t>(input.step_seconds) * 1'000'000'000;
    if (static_cast<double>(last - first) * 1e-9 > input.step_seconds * 10000.0)
        throw std::invalid_argument("Display cache exceeds 10001 nodes; shorten the interval");
    const auto sample = [&](core::Epoch epoch) {
        check_cancel();
        const auto result = session.sample(std::array{epoch});
        return result.states().front();
    };
    std::vector<core::CartesianState> states;
    std::vector<core::CartesianState> inertial_states;
    double min_radius = std::numeric_limits<double>::infinity(), max_radius = 0;
    for (auto ns = first;;) {
        auto inertial = sample(core::Epoch{ns});
        auto state =
            frames.gcrf_to_itrf_state(inertial.epoch, eop.at(inertial.epoch)).apply(inertial);
        inertial_states.push_back(inertial);
        const auto &r = state.position_m;
        const auto radius = std::hypot(r[0], r[1], r[2]);
        min_radius = std::min(min_radius, radius);
        max_radius = std::max(max_radius, radius);
        states.push_back(std::move(state));
        if (ns == last)
            break;
        // The bounded duration above makes the subtraction safe.
        ns += std::min(step, last - ns);
    }
    const auto provenance = session.sample(std::array{input.first}).provenance();
    visualization::StateTrack track{results::StateSeries{provenance, std::move(states)}};
    visualization::StateTrack inertial_track{
        results::StateSeries{provenance, std::move(inertial_states)}};
    double max_error = 0;
    double max_inertial_error = 0;
    const auto &nodes = track.samples().states();
    for (std::size_t i = 1; i < nodes.size(); ++i) {
        const auto a = nodes[i - 1].epoch.tai_nanoseconds(), b = nodes[i].epoch.tai_nanoseconds();
        for (const auto numerator : {1, 2, 3}) {
            const core::Epoch epoch{a + (b - a) * numerator / 4};
            const auto direct = sample(epoch);
            const auto interpolated = track.sample(epoch).markers.front().position_m;
            // Quarter-segment validation needs positions, not the costly R-dot velocity
            // transform. Cache nodes still use the full state transform for Hermite tangents.
            const auto r =
                frames.gcrf_to_itrf(epoch, eop.at(epoch).sample).apply_position(direct.position_m);
            const auto error =
                std::hypot(r[0] - interpolated[0], r[1] - interpolated[1], r[2] - interpolated[2]);
            max_error = std::max(max_error, error);
            const auto inertial_interpolated =
                inertial_track.sample(epoch).markers.front().position_m;
            const auto inertial_error = std::hypot(direct.position_m[0] - inertial_interpolated[0],
                                                   direct.position_m[1] - inertial_interpolated[1],
                                                   direct.position_m[2] - inertial_interpolated[2]);
            max_inertial_error = std::max(max_inertial_error, inertial_error);
            if (error > 1 || inertial_error > 1)
                throw std::runtime_error(
                    "Display interpolation check exceeds 1 m; reduce the display step");
        }
    }
    check_cancel();
    auto eop_source = eop.at(nodes.front().epoch).sample.source_id;
    if (info.extrapolation_days > 0)
        eop_source +=
            ";policy=endpoint-linear-tai-v1;limit-days=" + std::to_string(info.extrapolation_days) +
            ";analysis-first-tai-ns=" + std::to_string(analysis_first) +
            ";analysis-last-tai-ns=" + std::to_string(analysis_last);
    return std::make_shared<const OrbitScene>(OrbitScene{
        std::move(input), std::move(session), std::move(track), std::move(inertial_track),
        max_error, max_inertial_error, min_radius, max_radius, eop_source,
        std::chrono::duration<double>(std::chrono::steady_clock::now() - started).count()});
}
bool cache_covers(const OrbitScene &orbit, core::Epoch first, core::Epoch last) {
    const auto &nodes = orbit.track.samples().states();
    return nodes.front().epoch.tai_nanoseconds() <= first.tai_nanoseconds() &&
           nodes.back().epoch.tai_nanoseconds() >= last.tai_nanoseconds();
}
} // namespace orbitforge::desktop
