#pragma once
#include <orbelle/core/time/time_provider.hpp>
#include <string>

namespace orbelle::core {
// One fixed element set. Lines include their NORAD checksums, exactly 69 ASCII bytes each.
struct Tle {
    std::string line1, line2;
    bool operator==(const Tle &) const = default;
};
struct TleElements {
    std::string catalog_id; // Five characters, numeric IDs normalized with leading zeros.
    UtcDateTime epoch_utc;
    double epoch_days_since_1950; // UTC calendar days since 1949-12-31, for SGP4 initialization.
    double inclination_rad, raan_rad, eccentricity, argument_of_periapsis_rad, mean_anomaly_rad;
    double mean_motion_revolutions_per_day;
    double mean_motion_dot_over_two, mean_motion_ddot_over_six; // Raw TLE fields, rev/day^n.
    double bstar;
};
// Strict fixed-column parsing; validates checksums, matching catalog IDs, dates and ranges.
// Alpha-5 catalog IDs are accepted. No I/O and no implicit time-data lookup.
[[nodiscard]] TleElements parse_tle(const Tle &tle);
} // namespace orbelle::core
