#pragma once
#include <atomic>
#include <memory>
#include <optional>
#include <orbitforge/application/session.hpp>
#include <orbitforge/core/frames/frame_provider.hpp>
#include <orbitforge/visualization/state_track.hpp>

namespace orbitforge::desktop {
struct OrbitInput {
    core::CartesianState initial{
        core::Epoch{0}, core::OriginId::earth, core::FrameId::gcrf, {7e6, 0, 0}, {0, 6200, 5300}};
    double mu = 3.986004418e14;
    std::string object_id = "satellite-1";
    core::Epoch first{0}, last{7'200'000'000'000};
    int step_seconds = 60;
};
struct OrbitScene {
    OrbitInput input;
    application::Session session;
    visualization::StateTrack track;
    visualization::StateTrack inertial_track;
    double max_check_error_m;
    double max_inertial_check_error_m;
    double min_radius_m, max_radius_m;
    std::string eop_source_id;
    double preparation_seconds;
};
class OrbitCancelled : public std::runtime_error {
  public:
    OrbitCancelled() : std::runtime_error("Orbit preparation cancelled") {}
};
// CPU-only preparation; no Qt/Vulkan objects and no publication side effects.
[[nodiscard]] std::shared_ptr<const OrbitScene> prepare_orbit_scene(
    OrbitInput input, application::Session session, const core::IFrameProvider &frames,
    const core::IEarthOrientationProvider &eop, const std::atomic_bool &cancelled,
    std::optional<std::pair<core::Epoch, core::Epoch>> cache_interval = std::nullopt);
[[nodiscard]] bool cache_covers(const OrbitScene &orbit, core::Epoch first, core::Epoch last);
} // namespace orbitforge::desktop
