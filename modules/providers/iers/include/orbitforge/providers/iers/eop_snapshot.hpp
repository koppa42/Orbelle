#pragma once
#include <filesystem>
#include <memory>
#include <orbitforge/core/frames/eop_provider.hpp>

namespace orbitforge::providers::iers {
// The loader takes ownership of validated values, never of the file or time provider.
[[nodiscard]] std::unique_ptr<core::IEarthOrientationProvider>
load_eop_snapshot(const std::filesystem::path &manifest, const core::ITimeProvider &time);
// Copies a validated manifest and its data into a content-addressed directory.
// Publication is atomic; an existing snapshot is revalidated, never overwritten.
[[nodiscard]] std::filesystem::path import_eop_snapshot(const std::filesystem::path &manifest,
                                                        const std::filesystem::path &store,
                                                        const core::ITimeProvider &time);
} // namespace orbitforge::providers::iers
