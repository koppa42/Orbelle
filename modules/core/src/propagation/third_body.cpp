#include <cmath>
#include <orbitforge/core/propagation/third_body.hpp>
#include <stdexcept>

namespace orbitforge::core {
std::string_view third_body_name(ThirdBody body) {
    constexpr std::array<std::string_view, third_body_count> names{
        "Sun",  "Moon",    "Jupiter", "Venus",   "Saturn",
        "Mars", "Mercury", "Uranus",  "Neptune", "Pluto"};
    return names.at(static_cast<std::size_t>(body));
}
std::int32_t third_body_naif_id(ThirdBody body) {
    constexpr std::array<std::int32_t, third_body_count> ids{10, 301, 5, 299, 6, 4, 199, 7, 8, 9};
    return ids.at(static_cast<std::size_t>(body));
}
double ThirdBodyDataInfo::mu(ThirdBody body) const {
    if (body == ThirdBody::sun)
        return sun_mu;
    if (body == ThirdBody::moon)
        return moon_mu;
    return planet_mu.at(static_cast<std::size_t>(body) - 2);
}
double third_body_mu(const ThirdBodyGravity &settings, ThirdBody body,
                     const ThirdBodyDataInfo &data) {
    double value = 0;
    switch (settings.source) {
    case ThirdBodyGravitySource::jpl_de:
        value = data.mu(body);
        break;
    case ThirdBodyGravitySource::cb_file:
        if (!settings.cb)
            throw std::invalid_argument("Cb file gravity requires imported constants");
        if (settings.cb->body != body)
            throw std::invalid_argument("Cb file belongs to another body");
        value = settings.cb->mu;
        break;
    case ThirdBodyGravitySource::user_specified:
        if (!settings.user_mu)
            throw std::invalid_argument("User Specified gravity requires a GM value");
        value = *settings.user_mu;
        break;
    default:
        throw std::invalid_argument("Unsupported third-body gravity source");
    }
    if (!std::isfinite(value) || value <= 0)
        throw std::invalid_argument(std::string(third_body_name(body)) +
                                    " GM must be finite and positive");
    return value;
}
std::array<double, 3> third_body_acceleration(const std::array<double, 3> &r,
                                              const std::array<double, 3> &rb, double mu) {
    if (!std::isfinite(mu) || mu <= 0)
        throw std::invalid_argument("Third-body GM must be finite and positive");
    const double distance = std::hypot(rb[0], rb[1], rb[2]);
    if (!(distance > 0) || !std::isfinite(distance))
        throw std::invalid_argument("Third-body position must be finite and nonzero");
    std::array<double, 3> delta{}, u{}, v{};
    for (int i = 0; i < 3; ++i) {
        if (!std::isfinite(r[i]) || !std::isfinite(rb[i]))
            throw std::invalid_argument("Third-body positions must be finite");
        delta[i] = r[i] - rb[i];
        u[i] = r[i] / distance;
        v[i] = rb[i] / distance;
    }
    const double separation = std::hypot(delta[0], delta[1], delta[2]);
    if (!(distance > 0) || !std::isfinite(distance) || !(separation > 0) ||
        !std::isfinite(separation))
        throw std::invalid_argument("Third-body gravity is singular at a body centre");
    // q = |r-rb|^2/|rb|^2 - 1, without subtracting two nearly equal distances.
    double q = 0;
    for (int i = 0; i < 3; ++i)
        q += u[i] * (u[i] - 2 * v[i]);
    // f = (1+q)^(3/2)-1, rationalized to preserve the tidal term near r=0.
    const double ratio = separation / distance;
    const double cube = ratio * ratio * ratio;
    const double f = q * (3 + q * (3 + q)) / (1 + cube);
    const double scale = -mu / separation / separation / separation;
    std::array<double, 3> result{};
    for (int i = 0; i < 3; ++i) {
        result[i] = scale * (r[i] + f * rb[i]);
        if (!std::isfinite(result[i]))
            throw std::invalid_argument("Third-body acceleration is not finite");
    }
    return result;
}
} // namespace orbitforge::core
