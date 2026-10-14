#pragma once
#include <array>
#include <orbitforge/core/time/epoch.hpp>
#include <string>

namespace orbitforge::core {
enum class ThirdBody { sun, moon };
struct ThirdBodyDataInfo {
    std::string model, content_sha256, interpretation_version;
    Epoch first{0}, last{0};        // Inclusive TAI interval; no extrapolation.
    double sun_mu = 0, moon_mu = 0; // m^3/s^2, matching the ephemeris solution.
};
// Geometric Earth-relative positions in ICRF-aligned axes for Newtonian forces.
// This is not a full relativistic BCRS -> GCRS state conversion; no aberration,
// light time or velocity re-labelling. Implementations own immutable resources.
class IThirdBodyEphemeris {
  public:
    virtual ~IThirdBodyEphemeris() = default;
    [[nodiscard]] virtual std::array<double, 3> position(ThirdBody body, Epoch epoch) const = 0;
    virtual void validate_interval(Epoch first, Epoch last, bool sun, bool moon) const = 0;
    [[nodiscard]] virtual const ThirdBodyDataInfo &data_info() const noexcept = 0;
};
// Differential gravity: includes the acceleration of the geocentric origin.
// SI positions in the same inertial axes, positive finite GM. Stable for |r| << |rb|.
[[nodiscard]] std::array<double, 3> third_body_acceleration(const std::array<double, 3> &satellite,
                                                            const std::array<double, 3> &body,
                                                            double mu);
} // namespace orbitforge::core
