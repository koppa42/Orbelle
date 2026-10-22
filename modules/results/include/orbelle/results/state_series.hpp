#pragma once
#include <cstdint>
#include <orbelle/core/frames/state.hpp>
#include <string>
#include <vector>

namespace orbelle::results {
struct Provenance {
    std::string object_id;
    std::uint64_t input_revision;
    std::string algorithm;
};
class StateSeries {
  public:
    StateSeries(Provenance provenance, std::vector<core::CartesianState> states);
    [[nodiscard]] const Provenance &provenance() const noexcept;
    [[nodiscard]] const std::vector<core::CartesianState> &states() const noexcept;

  private:
    Provenance provenance_;
    std::vector<core::CartesianState> states_;
};
} // namespace orbelle::results
