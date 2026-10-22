#pragma once
#include <cstdint>

namespace orbelle::core {
// Continuous TAI time: zero is 2000-01-01 12:00:00 TAI (not J2000 TT).
// Calendar and time-scale conversion belong to an injected ITimeProvider.
class Epoch {
  public:
    explicit constexpr Epoch(std::int64_t tai_nanoseconds_since_2000_noon) noexcept
        : ticks_(tai_nanoseconds_since_2000_noon) {}
    [[nodiscard]] constexpr std::int64_t tai_nanoseconds() const noexcept { return ticks_; }
    [[nodiscard]] constexpr double seconds_since(Epoch other) const noexcept {
        constexpr std::int64_t billion = 1'000'000'000;
        // Split before subtracting: even extreme valid epochs cannot overflow.
        const auto seconds = ticks_ / billion - other.ticks_ / billion;
        const auto nanos = ticks_ % billion - other.ticks_ % billion;
        return static_cast<double>(seconds) + static_cast<double>(nanos) / 1.0e9;
    }
    friend constexpr bool operator==(Epoch, Epoch) = default;

  private:
    std::int64_t ticks_;
};
} // namespace orbelle::core
