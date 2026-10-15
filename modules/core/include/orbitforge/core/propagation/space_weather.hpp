#pragma once
#include <array>
#include <memory>
#include <optional>
#include <orbitforge/core/time/time_provider.hpp>
#include <span>
#include <vector>

namespace orbitforge::core {
enum class AtmosphereModel { jacchia_roberts, nrlmsise00 };
enum class GeomagneticUpdate { daily, three_hourly };
enum class GeomagneticSource { ap, kp };
enum class WeatherQuality { observed, interpolated, daily_prediction, monthly_prediction };
struct SpaceWeatherDay {
    UtcDateTime date{2000, 1, 1, 0, 0, 0, 0};
    std::array<std::optional<double>, 8> kp{}, ap{};
    std::optional<double> daily_kp, daily_ap;
    // sfu. Observed and 1-AU-adjusted fluxes are distinct; centered and trailing
    // 81-day means are distinct. Missing input remains missing, never zero-filled.
    std::optional<double> f107_observed, f107_adjusted;
    std::optional<double> observed_center81, observed_last81;
    std::optional<double> adjusted_center81, adjusted_last81;
    std::optional<int> flux_qualifier; // Original fixed-width Q flag, when present.
    WeatherQuality quality = WeatherQuality::observed;
};
struct SpaceWeatherInfo {
    std::string sha256, source_url, format;
    UtcDateTime first{}, last{}; // Inclusive raw record dates, not propagation coverage.
};
struct SpaceWeatherSample {
    double f107, f107_average, kp, ap;
    std::optional<std::array<double, 7>> ap_history;
};
class SpaceWeatherCoverageError : public std::out_of_range {
    using std::out_of_range::out_of_range;
};
// Immutable parsed weather. Thread-safe, owns all values, no file or network access.
// Queries require an explicit UTC provider: lag durations and interpolation use
// elapsed TAI seconds, while daily / three-hour windows follow UTC boundaries.
class SpaceWeather {
  public:
    SpaceWeather(SpaceWeatherInfo info, std::vector<SpaceWeatherDay> days);
    [[nodiscard]] const SpaceWeatherInfo &info() const noexcept { return info_; }
    [[nodiscard]] std::span<const SpaceWeatherDay> days() const noexcept { return days_; }
    // F10.7: observed values at 20:00 UTC, linear interpolation. Daily flux lag
    // 86400 s for both models. Average flux lag: JR 86400 s, NRL 0 s.
    // Geomagnetic lag: JR 24120 s, NRL 0 s. NRL three-hour mode assembles the
    // native seven-value Ap history; JR uses the selected scalar Kp / converted Ap.
    [[nodiscard]] SpaceWeatherSample sample(Epoch at, const ITimeProvider &time,
                                            AtmosphereModel model, GeomagneticUpdate update,
                                            GeomagneticSource source) const;
    // Checks every required record / slot, including all lags and history, not
    // just the two endpoints. Missing fields, holes and extrapolation are errors.
    void validate_interval(Epoch first, Epoch last, const ITimeProvider &time,
                           AtmosphereModel model, GeomagneticUpdate update,
                           GeomagneticSource source) const;

  private:
    SpaceWeatherInfo info_;
    std::vector<SpaceWeatherDay> days_;
    std::vector<int> day_numbers_;
    [[nodiscard]] const SpaceWeatherDay &day(int number) const;
};
[[nodiscard]] double ap_to_kp(double ap);
} // namespace orbitforge::core
