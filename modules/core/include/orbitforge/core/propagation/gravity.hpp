#pragma once
#include <array>
#include <memory>
#include <span>
#include <string>
#include <vector>

namespace orbitforge::core {
// The file may contain more coefficients; this is the independently validated
// computation limit. Coefficients are fully normalized, without Condon-Shortley phase.
inline constexpr int gravity_max_degree = 360;
struct GravityInfo {
    std::string model, sha256, source_url, tide_system;
    double mu = 0, radius_m = 0;
    int degree = 0, order = 0;
};
struct GravityCoefficient {
    int degree = 0, order = 0;
    double c = 0, s = 0;
};
// Immutable, Earth-centered static potential, including its monopole. SI units;
// input/output axes are Earth-fixed. Optional additive normalized corrections are supplied
// by the caller at the force epoch; they never mutate the immutable base field.
class GravityModel {
  public:
    GravityModel(GravityInfo info, const std::vector<GravityCoefficient> &coefficients);
    [[nodiscard]] const GravityInfo &info() const noexcept { return info_; }
    [[nodiscard]] std::array<double, 3>
    acceleration(const std::array<double, 3> &position_m, int degree, int order,
                 std::span<const GravityCoefficient> corrections = {}) const;

  private:
    GravityInfo info_;
    struct Term {
        double c = 0, s = 0, a = 0, b = 0;
    };
    std::vector<Term> terms_;
};
[[nodiscard]] std::shared_ptr<const GravityModel> builtin_egm96();
} // namespace orbitforge::core
