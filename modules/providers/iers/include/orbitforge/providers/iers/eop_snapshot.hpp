#pragma once
#include <filesystem>
#include <memory>
#include <orbitforge/core/frames/eop_provider.hpp>

namespace orbitforge::providers::iers {
// The loader takes ownership of validated values, never of the file or time provider.
[[nodiscard]] std::unique_ptr<core::IEarthOrientationProvider>
load_eop_snapshot(const std::filesystem::path &manifest, const core::ITimeProvider &time);
} // namespace orbitforge::providers::iers
