#pragma once
#include <orbelle/core/time/time_provider.hpp>
#include <string>

namespace orbelle::core {
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
    Epoch last_epoch; // Inclusive original coverage; never extended by a usage policy.
    TimeDataInfo time_data;
    int extrapolation_days = 0; // 0 disables; otherwise endpoint-linear-tai-v1, both ends.
};
inline constexpr int max_eop_extrapolation_days = 30;
[[nodiscard]] inline bool eop_covers(const EopDataInfo &info, Epoch epoch) noexcept {
    return info.extrapolation_days >= 0 && info.extrapolation_days <= max_eop_extrapolation_days &&
           info.first_epoch.tai_nanoseconds() < info.last_epoch.tai_nanoseconds() &&
           epoch.seconds_since(info.first_epoch) >= -info.extrapolation_days * 86400.0 &&
           epoch.seconds_since(info.last_epoch) <= info.extrapolation_days * 86400.0;
}
class IEarthOrientationProvider {
  public:
    virtual ~IEarthOrientationProvider() = default;
    [[nodiscard]] virtual EarthOrientationState at(Epoch epoch) const = 0;
    [[nodiscard]] virtual const EopDataInfo &data_info() const noexcept = 0;
};
} // namespace orbelle::core
