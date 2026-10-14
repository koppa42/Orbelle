#pragma once
#include <filesystem>
#include <memory>
#include <orbitforge/application/data_lock.hpp>
#include <orbitforge/core/ephemeris/ephemeris_provider.hpp>
#include <orbitforge/core/propagation/third_body.hpp>

namespace orbitforge::runtime {
[[nodiscard]] bool ephemeris_available() noexcept;
[[nodiscard]] std::unique_ptr<core::IEphemerisProvider>
load_spice_snapshot(const std::filesystem::path &manifest, const std::filesystem::path &workspace);
// Explicit Newtonian position adapter for DE440 resources, never a CartesianState relabel.
[[nodiscard]] std::shared_ptr<const core::IThirdBodyEphemeris>
make_third_body_ephemeris(std::shared_ptr<const core::IEphemerisProvider> ephemeris);
[[nodiscard]] application::DataReference ephemeris_reference(const std::string &selection,
                                                             const core::ThirdBodyDataInfo &info);
void validate_ephemeris_reference(std::span<const application::DataReference> recorded,
                                  const std::string &selection,
                                  const core::ThirdBodyDataInfo &info);
inline constexpr double de440_sun_mu = 1.3271244004127942e20;
inline constexpr double de440_moon_mu = 4.9028001184575496e12;
inline constexpr auto de440s_sha256 =
    "c1c7feeab882263fc493a9d5a5b2ddd71b54826cdf65d8d17a76126b260a49f2";
inline constexpr auto de440s_url =
    "https://naif.jpl.nasa.gov/pub/naif/generic_kernels/spk/planets/de440s.bsp";
} // namespace orbitforge::runtime
