#include <algorithm>
#include <cmath>
#include <complex>
#include <orbitforge/core/propagation/solid_tides.hpp>
#include <stdexcept>

namespace orbitforge::core {
namespace {
constexpr double k20 = 0.30190;
constexpr std::array k2_real{k20, 0.29830, 0.30102};
constexpr std::array k2_imag{0.0, -0.00144, -0.00130};
constexpr std::array k2_plus{-0.00089, -0.00080, -0.00057};
constexpr std::array k3_real{0.093, 0.093, 0.093, 0.094};
} // namespace
double iers2003_permanent_tide_c20() noexcept { return 4.4228e-8 * -0.31460 * k20; }
void validate_solid_tide_options(const SolidTideOptions &options) {
    if (options.mode != SolidTideMode::none && options.mode != SolidTideMode::permanent &&
        options.mode != SolidTideMode::full)
        throw std::invalid_argument("Unsupported solid tide mode");
}
SolidTideOptions effective_solid_tide_options(SolidTideOptions options, int gravity_degree) {
    validate_solid_tide_options(options);
    if (gravity_degree == 0 && options.truncate_to_gravity)
        return {};
    if (options.mode == SolidTideMode::none ||
        (options.mode == SolidTideMode::permanent && gravity_degree >= 2))
        options.truncate_to_gravity = true;
    return options;
}
std::vector<GravityCoefficient> solid_tide_corrections(const GravityInfo &field,
                                                       const SolidTideOptions &options, int degree,
                                                       int order,
                                                       std::span<const TideRaisingBody> bodies) {
    validate_solid_tide_options(options);
    if (!std::isfinite(field.mu) || field.mu <= 0 || !std::isfinite(field.radius_m) ||
        field.radius_m <= 0 ||
        (field.tide_system != "tide_free" && field.tide_system != "zero_tide"))
        throw std::invalid_argument("Solid tides require a tide-free or zero-tide Earth field");
    if (degree < 0 || degree == 1 || degree > std::min(field.degree, gravity_max_degree) ||
        order < 0 || order > std::min(degree, field.order))
        throw std::invalid_argument("Invalid gravity truncation for solid tides");
    if (options.truncate_to_gravity && degree == 0)
        return {};
    std::array<std::array<std::complex<double>, 4>, 5> delta{};
    if (options.mode == SolidTideMode::permanent)
        delta[2][0] = iers2003_permanent_tide_c20();
    else if (options.mode == SolidTideMode::full) {
        if (bodies.size() != 2)
            throw std::invalid_argument("Full solid tides require geometric Sun and Moon states");
        for (const auto &body : bodies) {
            const auto &r = body.position_itrf_m;
            const double distance = std::hypot(r[0], r[1], r[2]);
            if (!std::isfinite(distance) || distance <= field.radius_m || !std::isfinite(body.mu) ||
                body.mu <= 0)
                throw std::invalid_argument("Invalid solid-tide raising body geometry or GM");
            const double z = r[2] / distance;
            const std::complex<double> xy{r[0] / distance, r[1] / distance};
            // Explicit fully normalized Cartesian surface harmonics, no longitude
            // division. Well-defined even when a tide-raising body is at a pole.
            const std::array p2{std::complex<double>{std::sqrt(5.0) * (3 * z * z - 1) / 2},
                                std::sqrt(15.0) * z * xy, std::sqrt(15.0) / 2 * xy * xy};
            const std::array p3{std::complex<double>{std::sqrt(7.0) / 2 * z * (5 * z * z - 3)},
                                std::sqrt(21.0 / 8) * (5 * z * z - 1) * xy,
                                std::sqrt(105.0) / 2 * z * xy * xy,
                                std::sqrt(35.0 / 8) * xy * xy * xy};
            const double ratio = field.radius_m / distance;
            const double factor2 = body.mu / field.mu * ratio * ratio * ratio / 5;
            const double factor3 = factor2 * ratio * (5.0 / 7);
            for (std::size_t m = 0; m < p2.size(); ++m) {
                // C + i S = conjugate(k) * Pnm exp(i m lambda) * scale.
                delta[2][m] += std::complex<double>{k2_real[m], -k2_imag[m]} * factor2 * p2[m];
                delta[4][m] += k2_plus[m] * factor2 * p2[m];
            }
            for (std::size_t m = 0; m < p3.size(); ++m)
                delta[3][m] += k3_real[m] * factor3 * p3[m];
        }
    }
    if (field.tide_system == "zero_tide" && degree >= 2)
        delta[2][0] -= iers2003_permanent_tide_c20();
    std::vector<GravityCoefficient> result;
    result.reserve(10);
    for (int n = 2; n <= 4; ++n)
        for (int m = 0; m <= std::min(n, n == 4 ? 2 : 3); ++m) {
            if (options.truncate_to_gravity && (n > degree || m > order))
                continue;
            const auto value = delta[static_cast<std::size_t>(n)][static_cast<std::size_t>(m)];
            if (!std::isfinite(value.real()) || !std::isfinite(value.imag()))
                throw std::invalid_argument("Nonfinite solid tide correction");
            if (value != std::complex<double>{})
                result.push_back({n, m, value.real(), value.imag()});
        }
    return result;
}
} // namespace orbitforge::core
