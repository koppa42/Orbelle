#pragma once
#include <filesystem>
#include <memory>
#include <orbitforge/core/frames/frame_provider.hpp>
#include <orbitforge/core/time/time_provider.hpp>

namespace orbitforge::runtime {
[[nodiscard]] bool time_frames_available() noexcept;
// These factories throw std::runtime_error when ORBITFORGE_WITH_ERFA is disabled.
[[nodiscard]] std::unique_ptr<core::ITimeProvider> make_builtin_time_provider();
[[nodiscard]] std::unique_ptr<core::IFrameProvider> make_frame_provider();
[[nodiscard]] std::unique_ptr<core::IEarthOrientationProvider>
load_eop_snapshot(const std::filesystem::path &manifest);
} // namespace orbitforge::runtime
