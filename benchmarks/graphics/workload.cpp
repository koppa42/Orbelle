#include "workload.hpp"
#include <cmath>
#include <format>
#include <numbers>
#include <stdexcept>

namespace orbitforge::benchmarks {
namespace {
constexpr double mu = 3.986004418e14;
constexpr double equatorial_radius_m = 6378137.0;
constexpr double tau = 2 * std::numbers::pi;
constexpr std::int64_t second = 1'000'000'000;
} // namespace
Constellation::Constellation() {
    for (std::size_t i = 0; i < planes; ++i) {
        const auto band = static_cast<double>(i / 10);
        const double radius = equatorial_radius_m + 550000.0 + 40000.0 * band;
        const double inclination = (35.0 + 7.0 * band) * std::numbers::pi / 180.0;
        const double raan = tau * static_cast<double>(i % 10) / 10.0;
        planes_.push_back({core::CircularTwoBody(core::Length{radius},
                                                 core::GravitationalParameter{mu}, core::Epoch{0}),
                           std::cos(raan), std::sin(raan), std::cos(inclination),
                           std::sin(inclination)});
    }
}
visualization::RenderSnapshot Constellation::at(core::Epoch epoch) const {
    visualization::RenderSnapshot result{epoch, core::FrameId::gcrf, core::OriginId::earth, {}};
    result.markers.reserve(count);
    for (std::size_t i = 0; i < planes_.size(); ++i) {
        const auto &plane = planes_[i];
        const auto state = plane.orbit.state_at(epoch);
        for (std::size_t j = 0; j < satellites_per_plane; ++j) {
            // One slot of phasing per adjacent orbital plane.
            const double phase = tau * (static_cast<double>(j) + static_cast<double>(i) / planes) /
                                 satellites_per_plane;
            const double x =
                state.position_m[0] * std::cos(phase) - state.position_m[1] * std::sin(phase);
            const double y =
                state.position_m[0] * std::sin(phase) + state.position_m[1] * std::cos(phase);
            result.markers.push_back(
                {std::format("SAT-{:05}", i * satellites_per_plane + j),
                 {plane.cos_raan * x - plane.sin_raan * plane.cos_inclination * y,
                  plane.sin_raan * x + plane.cos_raan * plane.cos_inclination * y,
                  plane.sin_inclination * y}});
        }
    }
    return result;
}
DisplayCache::DisplayCache(const core::IFrameProvider &frames,
                           const core::IEarthOrientationProvider &eop) {
    const Constellation constellation;
    positions_.reserve((duration_seconds + 1) * Constellation::count);
    for (std::int64_t i = 0; i <= duration_seconds; ++i) {
        const core::Epoch epoch{i * second};
        const auto rotation = frames.gcrf_to_itrf(epoch, eop.at(epoch).sample);
        if (rotation.to != core::FrameId::itrf)
            throw std::invalid_argument("The display cache needs an explicit ITRF destination");
        const auto sample = visualization::rotate_snapshot(constellation.at(epoch), rotation);
        for (const auto &marker : sample.markers) {
            if (i == 0)
                ids_.push_back(marker.object_id);
            positions_.push_back(marker.position_m);
        }
    }
}
visualization::RenderSnapshot DisplayCache::sample(core::Epoch epoch) const {
    const auto ticks = epoch.tai_nanoseconds();
    if (ticks < 0 || ticks > duration_seconds * second)
        throw std::out_of_range("Rendering cache covers TAI +0 through +600 seconds only");
    const auto lower = static_cast<std::size_t>(ticks / second);
    const auto upper = std::min(lower + 1, static_cast<std::size_t>(duration_seconds));
    const double fraction = static_cast<double>(ticks % second) / second;
    visualization::RenderSnapshot result{epoch, core::FrameId::itrf, core::OriginId::earth, {}};
    result.markers.reserve(ids_.size());
    for (std::size_t i = 0; i < ids_.size(); ++i) {
        const auto &a = positions_[lower * ids_.size() + i];
        const auto &b = positions_[upper * ids_.size() + i];
        result.markers.push_back({ids_[i],
                                  {std::lerp(a[0], b[0], fraction), std::lerp(a[1], b[1], fraction),
                                   std::lerp(a[2], b[2], fraction)}});
    }
    return result;
}
std::vector<visualization::RenderSnapshot> DisplayCache::track(std::size_t object_index) const {
    if (object_index >= ids_.size())
        throw std::out_of_range("Unknown track object");
    std::vector<visualization::RenderSnapshot> samples;
    for (std::int64_t t = 0; t <= duration_seconds; t += 5)
        samples.push_back({core::Epoch{t * second},
                           core::FrameId::itrf,
                           core::OriginId::earth,
                           {{ids_[object_index], positions_[t * ids_.size() + object_index]}}});
    return samples;
}
std::size_t DisplayCache::position_bytes() const noexcept {
    return positions_.size() * sizeof(positions_.front());
}
} // namespace orbitforge::benchmarks
