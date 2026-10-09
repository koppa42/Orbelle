#pragma once
#include <filesystem>
#include <memory>
#include <orbitforge/core/ephemeris/ephemeris_provider.hpp>

namespace orbitforge::runtime {
[[nodiscard]] bool ephemeris_available() noexcept;
[[nodiscard]] std::unique_ptr<core::IEphemerisProvider>
load_spice_snapshot(const std::filesystem::path &manifest, const std::filesystem::path &workspace);
} // namespace orbitforge::runtime
