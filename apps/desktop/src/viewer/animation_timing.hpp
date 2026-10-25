#pragma once
#include <algorithm>
#include <cmath>
#include <cstdint>

namespace orbelle::desktop {
// Both clocks use the same monotonic nanosecond counter. Display cadence never
// determines simulated time, and paused time is discarded explicitly on resume/seek.
class PlaybackClock {
  public:
    void reset(std::int64_t now) { previous_ = now; }
    double advance(std::int64_t now) {
        const auto elapsed = now - previous_;
        previous_ = now;
        return static_cast<double>(elapsed) * 1e-9;
    }

  private:
    std::int64_t previous_ = 0;
};

class FramePacer {
  public:
    void reset() { period_ = deadline_ = 0; }
    int delay_ms(std::int64_t now, double refresh_rate) {
        if (!std::isfinite(refresh_rate) || refresh_rate <= 0)
            refresh_rate = 60;
        const auto period =
            static_cast<std::int64_t>(std::llround(1e9 / std::clamp(refresh_rate, 30.0, 240.0)));
        if (period != period_) {
            period_ = period;
            deadline_ = now + period_;
        } else {
            // Each completed frame consumes one slot, even if its timer fired a
            // little early. Never render twice against the same deadline.
            deadline_ += period_;
            if (deadline_ <= now)
                // Skip missed slots rather than queueing catch-up frames.
                deadline_ += ((now - deadline_) / period_ + 1) * period_;
        }
        // Carry fractional milliseconds in the deadline; rounding every period
        // independently would drift at e.g. 144/160/180 Hz. Always yield to Qt input.
        return static_cast<int>((deadline_ - now + 999'999) / 1'000'000);
    }

  private:
    std::int64_t period_ = 0, deadline_ = 0;
};
} // namespace orbelle::desktop
