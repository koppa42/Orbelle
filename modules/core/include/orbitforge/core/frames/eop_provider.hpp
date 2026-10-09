#pragma once
#include <orbitforge/core/time/time_provider.hpp>
#include <string>

namespace orbitforge::core {
class EopDataError : public std::runtime_error {
  public:
    using std::runtime_error::runtime_error;
};
class EopCoverageError : public std::out_of_range {
  public:
    using std::out_of_range::out_of_range;
};

struct EarthOrientationSample {
    Epoch epoch;
    double ut1_minus_tai_seconds;
    double polar_motion_x_rad;
    double polar_motion_y_rad;
    double celestial_pole_dx_rad;
    double celestial_pole_dy_rad;
    std::string source_id;
};
// Derivatives with respect to one continuous TAI second, in the same field order.
struct EarthOrientationRates {
    double ut1_minus_tai_seconds_per_second;
    double polar_motion_x_rad_per_second;
    double polar_motion_y_rad_per_second;
    double celestial_pole_dx_rad_per_second;
    double celestial_pole_dy_rad_per_second;
};
struct EarthOrientationState {
    EarthOrientationSample sample;
    EarthOrientationRates rates;
};
struct EopDataInfo {
    std::string id;
    std::string sha256;
    std::string source_url;
    std::string reference_frame;
    std::string interpolation;
    Epoch first_epoch;
    Epoch last_epoch; // Inclusive; no extrapolation past this sample.
    TimeDataInfo time_data;
};
class IEarthOrientationProvider {
  public:
    virtual ~IEarthOrientationProvider() = default;
    [[nodiscard]] virtual EarthOrientationState at(Epoch epoch) const = 0;
    [[nodiscard]] virtual const EopDataInfo &data_info() const noexcept = 0;
};
} // namespace orbitforge::core
