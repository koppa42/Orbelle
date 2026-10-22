#pragma once
#include <chrono>
#include <orbelle/core/propagation/access.hpp>
#include <orbelle/core/time/epoch.hpp>
#include <string>
#include <vector>

namespace orbelle::results {
// Exact TAI interval. A clipped edge is the query boundary, not an observed transition.
class AccessWindow {
  public:
    AccessWindow(core::Epoch start, core::Epoch end, bool start_clipped, bool end_clipped,
                 std::optional<core::PassExtrema> extrema = {});
    [[nodiscard]] core::Epoch start() const { return start_; }
    [[nodiscard]] core::Epoch end() const { return end_; }
    [[nodiscard]] std::chrono::nanoseconds duration() const { return duration_; }
    [[nodiscard]] bool start_clipped() const { return start_clipped_; }
    [[nodiscard]] bool end_clipped() const { return end_clipped_; }

    [[nodiscard]] const auto &extrema() const { return extrema_; }

  private:
    std::optional<core::PassExtrema> extrema_;
    core::Epoch start_, end_;
    std::chrono::nanoseconds duration_;
    bool start_clipped_, end_clipped_;
};
struct AccessQuery {
    core::Epoch first, last;
    std::chrono::nanoseconds search_step, boundary_tolerance;
    core::AccessSearchMode search_mode = core::AccessSearchMode::extremum_assisted;
};
struct AccessStatistics {
    std::size_t count = 0;
    double total_duration_s = 0, mean_duration_s = 0, shortest_duration_s = 0,
           longest_duration_s = 0;
};
struct AccessProvenance {
    std::string station_id, station_name, target_id, target_name;
    std::uint64_t input_revision;
    std::string input_sha256, data_sha256, algorithm;
};
// Owns its values, query parameters and provenance; no live project/provider references.
class AccessWindows {
  public:
    AccessWindows(std::vector<AccessWindow> windows, AccessQuery query,
                  AccessProvenance provenance);
    [[nodiscard]] const auto &windows() const { return windows_; }
    [[nodiscard]] AccessStatistics statistics() const;
    [[nodiscard]] const auto &query() const { return query_; }
    [[nodiscard]] const auto &provenance() const { return provenance_; }

  private:
    std::vector<AccessWindow> windows_;
    AccessQuery query_;
    AccessProvenance provenance_;
};
} // namespace orbelle::results
