#pragma once
#include <orbelle/results/state_series.hpp>
#include <utility>

namespace orbelle::results {
// An owned, read-only query result. Frame/origin remain defined for an empty query.
class Trajectory {
  public:
    Trajectory(StateSeries series, std::string object_name, core::OriginId origin,
               core::FrameId frame, std::string input_sha256, std::string data_sha256)
        : series_(std::move(series)), name_(std::move(object_name)), origin_(origin), frame_(frame),
          input_(std::move(input_sha256)), data_(std::move(data_sha256)) {}
    [[nodiscard]] const std::vector<core::CartesianState> &states() const {
        return series_.states();
    }
    [[nodiscard]] const Provenance &provenance() const { return series_.provenance(); }
    [[nodiscard]] const std::string &object_name() const { return name_; }
    [[nodiscard]] core::OriginId origin() const { return origin_; }
    [[nodiscard]] core::FrameId frame() const { return frame_; }
    [[nodiscard]] const std::string &input_sha256() const { return input_; }
    [[nodiscard]] const std::string &data_sha256() const { return data_; }

  private:
    StateSeries series_;
    std::string name_;
    core::OriginId origin_;
    core::FrameId frame_;
    std::string input_, data_;
};
} // namespace orbelle::results
