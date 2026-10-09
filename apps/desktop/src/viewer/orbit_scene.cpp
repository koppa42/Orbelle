#include "orbit_scene.hpp"
#include <algorithm>
#include <cmath>
#include <limits>

namespace orbitforge::desktop {
std::shared_ptr<const OrbitScene> prepare_orbit_scene(OrbitInput input,
                                                      application::Session session,
                                                      const core::IFrameProvider &frames,
                                                      const core::IEarthOrientationProvider &eop,
                                                      const std::atomic_bool &cancelled) {
    const auto check_cancel = [&] {
        if (cancelled.load())
            throw OrbitCancelled{};
    };
    check_cancel();
    const auto &info = eop.data_info();
    const auto first = input.first.tai_nanoseconds(), last = input.last.tai_nanoseconds();
    if (last <= first)
        throw std::invalid_argument("Analysis end must be later than its start");
    if (first < info.first_epoch.tai_nanoseconds() || last > info.last_epoch.tai_nanoseconds())
        throw std::out_of_range("Analysis interval is outside the installed EOP coverage");
    if (input.step_seconds < 1 || input.step_seconds > 60)
        throw std::invalid_argument("Display step must be between 1 and 60 seconds");
    const auto step = static_cast<std::int64_t>(input.step_seconds) * 1'000'000'000;
    if (input.last.seconds_since(input.first) > input.step_seconds * 10000.0)
        throw std::invalid_argument("Display cache exceeds 10001 nodes; shorten the interval");
    const auto sample = [&](core::Epoch epoch) {
        check_cancel();
        const auto result = session.sample(std::array{epoch});
        return frames.gcrf_to_itrf_state(epoch, eop.at(epoch)).apply(result.states().front());
    };
    std::vector<core::CartesianState> states;
    double min_radius = std::numeric_limits<double>::infinity(), max_radius = 0;
    for (auto ns = first;;) {
        auto state = sample(core::Epoch{ns});
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
    double max_error = 0;
    const auto &nodes = track.samples().states();
    for (std::size_t i = 1; i < nodes.size(); ++i) {
        const auto a = nodes[i - 1].epoch.tai_nanoseconds(), b = nodes[i].epoch.tai_nanoseconds();
        for (const auto numerator : {1, 2, 3}) {
            const core::Epoch epoch{a + (b - a) * numerator / 4};
            const auto direct = sample(epoch);
            const auto interpolated = track.sample(epoch).markers.front().position_m;
            const auto &r = direct.position_m;
            const auto error =
                std::hypot(r[0] - interpolated[0], r[1] - interpolated[1], r[2] - interpolated[2]);
            max_error = std::max(max_error, error);
            if (error > 1)
                throw std::runtime_error(
                    "Display interpolation check exceeds 1 m; reduce the display step");
        }
    }
    check_cancel();
    const auto eop_source = eop.at(nodes.front().epoch).sample.source_id;
    return std::make_shared<const OrbitScene>(OrbitScene{std::move(input), std::move(session),
                                                         std::move(track), max_error, min_radius,
                                                         max_radius, eop_source});
}
} // namespace orbitforge::desktop
