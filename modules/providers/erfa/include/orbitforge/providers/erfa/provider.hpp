#pragma once
#include <array>
#include <filesystem>
#include <memory>
#include <orbitforge/core/frames/frame_provider.hpp>
#include <orbitforge/core/time/time_provider.hpp>

namespace orbitforge::providers::erfa {
[[nodiscard]] std::unique_ptr<core::ITimeProvider> make_builtin_time_provider();
// IERS Leap_Second.dat; each instance owns an immutable, validated table.
[[nodiscard]] std::unique_ptr<core::ITimeProvider>
load_time_snapshot(const std::filesystem::path &file);
[[nodiscard]] std::unique_ptr<core::IFrameProvider> make_frame_provider();
// ERFA epv00 heliocentric Earth, reversed and normalized on ICRS/GCRF axes.
// TT substitutes for TDB; geometric display lighting only, not eclipse analysis.
// Throws outside epv00's supported 1900–2100 date interval.
[[nodiscard]] std::array<double, 3> display_sun_direction_gcrf(core::Epoch epoch);
} // namespace orbitforge::providers::erfa
