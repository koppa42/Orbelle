#pragma once
#include <array>
#include <filesystem>
#include <memory>
#include <orbelle/core/frames/frame_provider.hpp>
#include <orbelle/core/frames/geodetic.hpp>
#include <orbelle/core/time/time_provider.hpp>

namespace orbelle::providers::erfa {
[[nodiscard]] core::GeodeticPosition itrf_to_wgs84(core::CartesianState state);
[[nodiscard]] std::unique_ptr<core::ITimeProvider> make_builtin_time_provider();
// IERS Leap_Second.dat or NTP leap-seconds.list; owns an immutable, validated table.
// NTP #@ expiry is required; an optional #h checksum is verified. The original bytes
// determine SHA-256 identity. UTC conversion rejects the published expiry and later.
[[nodiscard]] std::unique_ptr<core::ITimeProvider>
load_time_snapshot(const std::filesystem::path &file);
[[nodiscard]] std::unique_ptr<core::IFrameProvider> make_frame_provider();
// ERFA epv00 heliocentric Earth, reversed and normalized on ICRS/GCRF axes.
// TT substitutes for TDB; geometric display lighting only, not eclipse analysis.
// Throws outside epv00's supported 1900–2100 date interval.
[[nodiscard]] std::array<double, 3> display_sun_direction_gcrf(core::Epoch epoch);
} // namespace orbelle::providers::erfa
