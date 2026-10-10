#include <orbitforge/runtime/time_frames.hpp>
#include <stdexcept>
#ifdef ORBITFORGE_HAS_ERFA
#include <orbitforge/providers/erfa/provider.hpp>
#include <orbitforge/providers/iers/eop_snapshot.hpp>
#endif

namespace orbitforge::runtime {
#ifdef ORBITFORGE_HAS_ERFA
namespace {
class LimitedEop final : public core::IEarthOrientationProvider {
  public:
    LimitedEop(std::shared_ptr<const core::IEarthOrientationProvider> source, int days,
               std::shared_ptr<const core::ITimeProvider> time)
        : source_(std::move(source)), time_(std::move(time)), info_(source_->data_info()) {
        if (info_.extrapolation_days != 0 || days < 1 || days > core::max_eop_extrapolation_days)
            throw std::invalid_argument("EOP extrapolation requires original data and 1..30 days");
        if (info_.time_data.sha256 != time_->data_info().sha256 ||
            info_.time_data.utc_valid_from_mjd != time_->data_info().utc_valid_from_mjd ||
            info_.time_data.utc_valid_until_mjd != time_->data_info().utc_valid_until_mjd)
            throw std::invalid_argument("EOP extrapolation time data mismatch");
        info_.extrapolation_days = days;
    }
    core::EarthOrientationState at(core::Epoch epoch) const override {
        if (!core::eop_covers(info_, epoch))
            throw core::EopCoverageError("Epoch exceeds the configured EOP extrapolation limit");
        (void)time_->to_utc(epoch); // Extrapolation never extends the leap-second table.
        if (epoch.tai_nanoseconds() >= info_.first_epoch.tai_nanoseconds() &&
            epoch.tai_nanoseconds() <= info_.last_epoch.tai_nanoseconds())
            return source_->at(epoch);
        const auto anchor = epoch.tai_nanoseconds() < info_.first_epoch.tai_nanoseconds()
                                ? info_.first_epoch
                                : info_.last_epoch;
        auto result = source_->at(anchor);
        const double dt = epoch.seconds_since(anchor);
        auto &sample = result.sample;
        const auto &rate = result.rates;
        sample.epoch = epoch;
        sample.ut1_minus_tai_seconds += dt * rate.ut1_minus_tai_seconds_per_second;
        sample.polar_motion_x_rad += dt * rate.polar_motion_x_rad_per_second;
        sample.polar_motion_y_rad += dt * rate.polar_motion_y_rad_per_second;
        sample.celestial_pole_dx_rad += dt * rate.celestial_pole_dx_rad_per_second;
        sample.celestial_pole_dy_rad += dt * rate.celestial_pole_dy_rad_per_second;
        sample.source_id += ";extrapolation=endpoint-linear-tai-v1;limit-days=" +
                            std::to_string(info_.extrapolation_days) +
                            ";offset-seconds=" + std::to_string(dt);
        return result;
    }
    const core::EopDataInfo &data_info() const noexcept override { return info_; }

  private:
    std::shared_ptr<const core::IEarthOrientationProvider> source_;
    std::shared_ptr<const core::ITimeProvider> time_;
    core::EopDataInfo info_;
};
} // namespace
#endif
std::unique_ptr<core::IEarthOrientationProvider>
with_eop_extrapolation(std::shared_ptr<const core::IEarthOrientationProvider> source, int days,
                       std::shared_ptr<const core::ITimeProvider> time) {
#ifdef ORBITFORGE_HAS_ERFA
    if (!source)
        throw std::invalid_argument("Missing EOP source");
    if (!time)
        time = make_builtin_time_provider();
    return std::make_unique<LimitedEop>(std::move(source), days, std::move(time));
#else
    (void)source;
    (void)days;
    (void)time;
    throw std::runtime_error("EOP extrapolation requires ORBITFORGE_WITH_ERFA=ON");
#endif
}
bool time_frames_available() noexcept {
#ifdef ORBITFORGE_HAS_ERFA
    return true;
#else
    return false;
#endif
}
std::unique_ptr<core::ITimeProvider> make_builtin_time_provider() {
#ifdef ORBITFORGE_HAS_ERFA
    return providers::erfa::make_builtin_time_provider();
#else
    throw std::runtime_error("Time provider requires a build with ORBITFORGE_WITH_ERFA=ON");
#endif
}
std::unique_ptr<core::IFrameProvider> make_frame_provider() {
#ifdef ORBITFORGE_HAS_ERFA
    return providers::erfa::make_frame_provider();
#else
    throw std::runtime_error("Frame provider requires a build with ORBITFORGE_WITH_ERFA=ON");
#endif
}
std::unique_ptr<core::ITimeProvider> load_time_snapshot(const std::filesystem::path &file) {
#ifdef ORBITFORGE_HAS_ERFA
    return providers::erfa::load_time_snapshot(file);
#else
    (void)file;
    throw std::runtime_error("Time data loading requires ORBITFORGE_WITH_ERFA=ON");
#endif
}
std::unique_ptr<core::IEarthOrientationProvider>
load_eop_snapshot_with_time(const std::filesystem::path &manifest,
                            const core::ITimeProvider &time) {
#ifdef ORBITFORGE_HAS_ERFA
    return providers::iers::load_eop_snapshot(manifest, time);
#else
    (void)manifest;
    (void)time;
    throw std::runtime_error("EOP loading requires ORBITFORGE_WITH_ERFA=ON");
#endif
}
std::unique_ptr<core::IEarthOrientationProvider>
load_eop_snapshot(const std::filesystem::path &manifest, int extrapolation_days) {
#ifdef ORBITFORGE_HAS_ERFA
    const auto time = make_builtin_time_provider();
    auto source = providers::iers::load_eop_snapshot(manifest, *time);
    return extrapolation_days == 0 ? std::move(source)
                                   : with_eop_extrapolation(std::move(source), extrapolation_days);
#else
    (void)manifest;
    (void)extrapolation_days;
    throw std::runtime_error("EOP loading requires a build with ORBITFORGE_WITH_ERFA=ON");
#endif
}
std::filesystem::path import_eop_snapshot(const std::filesystem::path &manifest,
                                          const std::filesystem::path &store) {
#ifdef ORBITFORGE_HAS_ERFA
    const auto time = make_builtin_time_provider();
    return providers::iers::import_eop_snapshot(manifest, store, *time);
#else
    (void)manifest;
    (void)store;
    throw std::runtime_error("EOP import requires a build with ORBITFORGE_WITH_ERFA=ON");
#endif
}
std::array<double, 3> display_sun_direction_gcrf(core::Epoch epoch) {
#ifdef ORBITFORGE_HAS_ERFA
    return providers::erfa::display_sun_direction_gcrf(epoch);
#else
    (void)epoch;
    throw std::runtime_error("Display sunlight requires ORBITFORGE_WITH_ERFA=ON");
#endif
}
} // namespace orbitforge::runtime
