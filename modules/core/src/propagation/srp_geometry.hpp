#pragma once
#include <orbelle/core/propagation/solar_radiation_pressure.hpp>

namespace orbelle::core::detail {
struct SrpGeometry {
    std::array<double, 3> sun{};
    std::optional<std::array<double, 3>> moon;
    std::array<SrpBodyPosition, third_body_count - 2> additional{};
    std::size_t count = 0;
};
// Options have already been validated by the caller. Nominal and variational
// paths share the same Sun method and geometric occultor-centre resolution.
template <class Ephemeris>
inline SrpGeometry resolve_srp_geometry(Epoch epoch, const std::array<double, 3> &r,
                                        const std::array<double, 3> &v, const SrpOptions &options,
                                        const Ephemeris &ephemeris,
                                        ApparentSunDifferential *partials = nullptr) {
    SrpGeometry result;
    switch (options.sun_position_method) {
    case SrpSunPositionMethod::geometric:
        result.sun = ephemeris.position(ThirdBody::sun, epoch);
        break;
    case SrpSunPositionMethod::apparent_to_true_cb:
        result.sun = ephemeris.apparent_sun_from_earth(epoch);
        break;
    case SrpSunPositionMethod::apparent:
        if (partials) {
            *partials = ephemeris.apparent_sun_from_spacecraft_differential(epoch, r, v);
            result.sun = partials->position_m;
        } else {
            result.sun = ephemeris.apparent_sun_from_spacecraft(epoch, r, v);
        }
        for (unsigned i = 0; i < 3; ++i)
            result.sun[i] += r[i];
        break;
    default:
        throw std::invalid_argument("Unsupported SRP Sun position method");
    }
    if (options.shadow_model != SrpShadowModel::none) {
        if (options.moon_shadow)
            result.moon = ephemeris.position(ThirdBody::moon, epoch);
        for (const auto &body : options.additional_eclipsing_bodies)
            result.additional[result.count++] = {
                body.body, ephemeris.eclipsing_body_position(body.body, epoch)};
    }
    return result;
}
} // namespace orbelle::core::detail
