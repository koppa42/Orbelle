#pragma once
#include <filesystem>
#include <memory>
#include <orbelle/core/frames/frame_provider.hpp>
#include <orbelle/core/frames/geodetic.hpp>
#include <orbelle/core/time/time_provider.hpp>
#include <span>

namespace orbelle::runtime {
struct EopCandidate {
    std::shared_ptr<const core::IEarthOrientationProvider> provider;
    int extrapolation_days = 0;
};
// Candidates are already loaded and ordered by preference. Shared by catalog and desktop.
[[nodiscard]] std::shared_ptr<const core::IEarthOrientationProvider>
select_eop_for_interval(std::span<const EopCandidate> candidates, core::Epoch first,
                        core::Epoch last, std::shared_ptr<const core::ITimeProvider> time);
[[nodiscard]] bool time_frames_available() noexcept;
// ERFA's WGS84 geocentric-to-geodetic conversion. Requires Earth-centred ITRF.
[[nodiscard]] core::GeodeticPosition itrf_to_wgs84(core::CartesianState state);
// These factories throw std::runtime_error when ORBELLE_WITH_ERFA is disabled.
[[nodiscard]] std::unique_ptr<core::ITimeProvider> make_builtin_time_provider();
[[nodiscard]] std::unique_ptr<core::ITimeProvider>
load_time_snapshot(const std::filesystem::path &file);
[[nodiscard]] std::unique_ptr<core::IFrameProvider> make_frame_provider();
// Approximate geometric sunlight for display; not an eclipse/visibility model.
[[nodiscard]] std::array<double, 3> display_sun_direction_gcrf(core::Epoch epoch);
[[nodiscard]] std::unique_ptr<core::IEarthOrientationProvider>
load_eop_snapshot(const std::filesystem::path &manifest, int extrapolation_days = 0);
[[nodiscard]] std::unique_ptr<core::IEarthOrientationProvider>
load_eop_snapshot_with_time(const std::filesystem::path &manifest, const core::ITimeProvider &time);
// Immutable policy wrapper; keeps original coverage/hash and enforces UTC validity.
[[nodiscard]] std::unique_ptr<core::IEarthOrientationProvider>
with_eop_extrapolation(std::shared_ptr<const core::IEarthOrientationProvider> source, int days,
                       std::shared_ptr<const core::ITimeProvider> time = {});
[[nodiscard]] std::filesystem::path import_eop_snapshot(const std::filesystem::path &manifest,
                                                        const std::filesystem::path &store);
} // namespace orbelle::runtime
