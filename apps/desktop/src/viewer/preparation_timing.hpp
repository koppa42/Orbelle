#pragma once
#include <chrono>
#include <cstddef>

namespace orbelle::desktop {
// Optional wall-clock diagnostics. A worker owns these fields until its future is ready.
struct PreparationTiming {
    double session_ms = 0, sampling_ms = 0, display_ms = 0, models_ms = 0;
    std::size_t sessions_acquired = 0, sessions_reused = 0, sample_calls = 0;
};
class StageTiming {
  public:
    using Clock = std::chrono::steady_clock;
    explicit StageTiming(double *total, const double *excluded = nullptr)
        : total_(total), excluded_(excluded), before_(excluded ? *excluded : 0),
          start_(total ? Clock::now() : Clock::time_point{}) {}
    StageTiming(const StageTiming &) = delete;
    StageTiming &operator=(const StageTiming &) = delete;
    ~StageTiming() { stop(); }
    void stop() {
        if (total_) {
            *total_ += std::chrono::duration<double, std::milli>(Clock::now() - start_).count() -
                       (excluded_ ? *excluded_ - before_ : 0);
            total_ = nullptr;
        }
    }

  private:
    double *total_;
    const double *excluded_;
    double before_;
    Clock::time_point start_;
};
} // namespace orbelle::desktop
