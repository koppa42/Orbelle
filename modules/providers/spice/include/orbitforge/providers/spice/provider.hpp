#pragma once
#include <filesystem>
#include <memory>
#include <orbitforge/core/ephemeris/ephemeris_provider.hpp>

namespace orbitforge::providers::spice {
// The caller supplies a writable workspace for owned temporary kernel copies.
// All CSPICE access to this linked instance must use this wrapper.
[[nodiscard]] std::unique_ptr<core::IEphemerisProvider>
load_snapshot(const std::filesystem::path &manifest, const std::filesystem::path &workspace);
} // namespace orbitforge::providers::spice
