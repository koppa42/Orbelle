#include <algorithm>
#include <cmath>
#include <limits>
#include <orbelle/results/access_windows.hpp>
#include <stdexcept>
#include <utility>

namespace orbelle::results {
AccessWindow::AccessWindow(core::Epoch start, core::Epoch end, bool start_clipped, bool end_clipped,
                           std::optional<core::PassExtrema> extrema)
    : extrema_(std::move(extrema)), start_(start), end_(end), start_clipped_(start_clipped),
      end_clipped_(end_clipped) {
    const auto a = start.tai_nanoseconds(), b = end.tai_nanoseconds();
    const auto span = static_cast<std::uint64_t>(b) - static_cast<std::uint64_t>(a);
    if (b <= a || span > static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max()))
        throw std::invalid_argument("Access window requires a positive int64 nanosecond duration");
    if (extrema_) {
        for (const auto &e : {extrema_->maximum_elevation, extrema_->minimum_range})
            if (!std::isfinite(e.value) || e.epoch.tai_nanoseconds() < a ||
                e.epoch.tai_nanoseconds() > b)
                throw std::invalid_argument("Pass extrema must be finite and within the pass");
        if (extrema_->minimum_range.value <= 0)
            throw std::invalid_argument("Pass range must be positive");
    }
    duration_ = std::chrono::nanoseconds{static_cast<std::int64_t>(span)};
}
AccessWindows::AccessWindows(std::vector<AccessWindow> windows, AccessQuery query,
                             AccessProvenance provenance)
    : windows_(std::move(windows)), query_(query), provenance_(std::move(provenance)) {
    if (query.first.tai_nanoseconds() > query.last.tai_nanoseconds() ||
        query.search_step.count() <= 0 || query.boundary_tolerance.count() <= 0 ||
        query.boundary_tolerance > query.search_step ||
        (query.search_mode != core::AccessSearchMode::fixed_grid &&
         query.search_mode != core::AccessSearchMode::extremum_assisted))
        throw std::invalid_argument("Invalid Access query interval, step or tolerance");
    auto previous_end = query.first;
    for (const auto &window : windows_) {
        if (window.start().tai_nanoseconds() < previous_end.tai_nanoseconds() ||
            window.end().tai_nanoseconds() > query.last.tai_nanoseconds())
            throw std::invalid_argument("Access windows must be ordered within the query interval");
        previous_end = window.end();
    }
}
AccessStatistics AccessWindows::statistics() const {
    AccessStatistics result;
    result.count = windows_.size();
    for (const auto &w : windows_) {
        const auto seconds = std::chrono::duration<double>(w.duration()).count();
        result.total_duration_s += seconds;
        result.longest_duration_s = std::max(result.longest_duration_s, seconds);
        if (result.shortest_duration_s == 0 || seconds < result.shortest_duration_s)
            result.shortest_duration_s = seconds;
    }
    if (result.count)
        result.mean_duration_s = result.total_duration_s / static_cast<double>(result.count);
    return result;
}
} // namespace orbelle::results
