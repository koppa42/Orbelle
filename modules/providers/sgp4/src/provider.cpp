#include "SGP4.h"
#include <cmath>
#include <numbers>
#include <orbelle/providers/sgp4/provider.hpp>

namespace orbelle::providers::sgp4 {
namespace {
[[noreturn]] void propagation_error(int code) {
    const char *reason = "initialization failed";
    switch (code) {
    case 1:
        reason = "mean eccentricity is outside the valid range";
        break;
    case 2:
        reason = "mean motion is nonpositive";
        break;
    case 3:
        reason = "perturbed eccentricity is outside the valid range";
        break;
    case 4:
        reason = "semi-latus rectum is negative";
        break;
    case 6:
        reason = "satellite has decayed below the reference surface";
        break;
    }
    throw core::PropagationError("SGP4 error " + std::to_string(code) + ": " + reason);
}
class Sgp4 final : public core::IStateProvider {
  public:
    Sgp4(const core::Tle &tle, const core::ITimeProvider &time) : epoch_(0) {
        const auto elements = core::parse_tle(tle);
        epoch_ = time.from_utc(elements.epoch_utc);
        const double radians_per_minute = 2 * std::numbers::pi / 1440;
        const bool ok = SGP4Funcs::sgp4init(
            wgs72, 'i', elements.catalog_id.c_str(), elements.epoch_days_since_1950, elements.bstar,
            elements.mean_motion_dot_over_two * radians_per_minute / 1440,
            elements.mean_motion_ddot_over_six * radians_per_minute / (1440 * 1440),
            elements.eccentricity, elements.argument_of_periapsis_rad, elements.inclination_rad,
            elements.mean_anomaly_rad,
            elements.mean_motion_revolutions_per_day * radians_per_minute, elements.raan_rad,
            initialized_);
        if (!ok || initialized_.error)
            propagation_error(initialized_.error);
        (void)state_at(epoch_);
    }
    core::CartesianState state_at(core::Epoch epoch) const override {
        // The reference routine mutates deep-space integration state. A local copy makes
        // random-order/backward/concurrent queries deterministic without a shared lock.
        auto record = initialized_;
        double r[3]{}, v[3]{};
        if (!SGP4Funcs::sgp4(record, epoch.seconds_since(epoch_) / 60, r, v) || record.error)
            propagation_error(record.error);
        core::CartesianState state{epoch, core::OriginId::earth, core::FrameId::teme, {}, {}};
        for (std::size_t i = 0; i < 3; ++i) {
            if (!std::isfinite(r[i]) || !std::isfinite(v[i]))
                throw core::PropagationError("SGP4 returned non-finite state");
            state.position_m[i] = r[i] * 1000;
            state.velocity_mps[i] = v[i] * 1000;
        }
        return state;
    }

  private:
    core::Epoch epoch_;
    elsetrec initialized_{};
};
} // namespace
std::shared_ptr<const core::IStateProvider> make_sgp4(const core::Tle &tle,
                                                      const core::ITimeProvider &time) {
    return std::make_shared<Sgp4>(tle, time);
}
} // namespace orbelle::providers::sgp4
