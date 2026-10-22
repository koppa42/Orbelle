#include <algorithm>
#include <cmath>
#include <orbelle/visualization/state_track.hpp>
#include <stdexcept>

namespace orbelle::visualization {
StateTrack::StateTrack(results::StateSeries samples) : samples_(std::move(samples)) {
    const auto &states = samples_.states();
    if (states.size() < 2)
        throw std::invalid_argument("A display track requires at least two states");
    for (std::size_t i = 0; i < states.size(); ++i) {
        const auto &s = states[i];
        if (s.frame != states.front().frame || s.origin != states.front().origin ||
            (i && s.epoch.tai_nanoseconds() <= states[i - 1].epoch.tai_nanoseconds()))
            throw std::invalid_argument("Display states need common axes and increasing epochs");
        for (std::size_t k = 0; k < 3; ++k)
            if (!std::isfinite(s.position_m[k]) || !std::isfinite(s.velocity_mps[k]))
                throw std::invalid_argument("Display state components must be finite");
    }
}
RenderSnapshot StateTrack::sample(core::Epoch epoch) const {
    const auto state = sample_state(epoch);
    return {
        epoch, state.frame, state.origin, {{samples_.provenance().object_id, state.position_m}}};
}
core::CartesianState StateTrack::sample_state(core::Epoch epoch) const {
    const auto &states = samples_.states();
    if (epoch.tai_nanoseconds() < states.front().epoch.tai_nanoseconds() ||
        epoch.tai_nanoseconds() > states.back().epoch.tai_nanoseconds())
        throw std::out_of_range("Requested epoch is outside the display track");
    const auto right =
        std::lower_bound(states.begin(), states.end(), epoch, [](const auto &s, auto t) {
            return s.epoch.tai_nanoseconds() < t.tai_nanoseconds();
        });
    if (right->epoch == epoch)
        return *right;
    const auto &a = *(right - 1), &b = *right;
    const double h = b.epoch.seconds_since(a.epoch), t = epoch.seconds_since(a.epoch) / h;
    const double t2 = t * t, t3 = t2 * t;
    auto result = a;
    result.epoch = epoch;
    auto &r = result.position_m;
    for (std::size_t k = 0; k < 3; ++k) {
        r[k] = (2 * t3 - 3 * t2 + 1) * a.position_m[k] + (t3 - 2 * t2 + t) * h * a.velocity_mps[k] +
               (-2 * t3 + 3 * t2) * b.position_m[k] + (t3 - t2) * h * b.velocity_mps[k];
        if (!std::isfinite(r[k]))
            throw std::runtime_error("Display interpolation overflow");
        result.velocity_mps[k] =
            (6 * t2 - 6 * t) / h * a.position_m[k] + (3 * t2 - 4 * t + 1) * a.velocity_mps[k] +
            (-6 * t2 + 6 * t) / h * b.position_m[k] + (3 * t2 - 2 * t) * b.velocity_mps[k];
    }
    return result;
}
std::vector<RenderSnapshot> StateTrack::nodes() const {
    std::vector<RenderSnapshot> snapshots;
    snapshots.reserve(samples_.states().size());
    for (std::size_t i = 0; i < samples_.states().size(); ++i)
        snapshots.push_back(make_snapshot(samples_, i));
    return snapshots;
}
} // namespace orbelle::visualization
