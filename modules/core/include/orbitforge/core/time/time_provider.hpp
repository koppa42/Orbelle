#pragma once
#include <orbitforge/core/time/epoch.hpp>
#include <stdexcept>
#include <string>

namespace orbitforge::core {
class TimeDataRangeError : public std::out_of_range {
  public:
    using std::out_of_range::out_of_range;
};

struct UtcDateTime {
    int year;
    int month;
    int day;
    int hour;
    int minute;
    int second; // 60 is allowed only during a positive leap second.
    int nanosecond;
    friend bool operator==(const UtcDateTime &, const UtcDateTime &) = default;
};

enum class TimeScale { tai, tt, tdb };
// Keep the two parts separate in numerical algorithms; their sum loses precision.
struct JulianDate {
    TimeScale scale;
    double day;
    double fraction;
};

struct TimeDataInfo {
    std::string id;
    std::string sha256;
    std::string source_url;
    int utc_valid_from_mjd;
    int utc_valid_until_mjd; // Exclusive, at UTC midnight.
};

class ITimeProvider {
  public:
    virtual ~ITimeProvider() = default;
    [[nodiscard]] virtual Epoch from_utc(const UtcDateTime &utc) const = 0;
    [[nodiscard]] virtual UtcDateTime to_utc(Epoch epoch) const = 0;
    [[nodiscard]] virtual int tai_minus_utc_seconds(Epoch epoch) const = 0;
    [[nodiscard]] virtual JulianDate julian_date(Epoch epoch, TimeScale scale) const = 0;
    [[nodiscard]] virtual const TimeDataInfo &data_info() const noexcept = 0;
};
} // namespace orbitforge::core
