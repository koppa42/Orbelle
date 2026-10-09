#include <orbitforge/runtime/two_body.hpp>
#ifdef ORBITFORGE_HAS_SPICE
#include <orbitforge/providers/spice/provider.hpp>
#endif
#include <stdexcept>
#include <utility>

namespace orbitforge::runtime {
bool two_body_available() noexcept {
#ifdef ORBITFORGE_HAS_SPICE
    return true;
#else
    return false;
#endif
}

core::CartesianState to_cartesian(core::KeplerianElements elements,
                                  core::GravitationalParameter mu) {
#ifdef ORBITFORGE_HAS_SPICE
    return providers::spice::to_cartesian(elements, mu);
#else
    (void)elements;
    (void)mu;
    throw std::runtime_error("Orbital element conversion requires ORBITFORGE_WITH_SPICE");
#endif
}
core::KeplerianElements to_keplerian(core::CartesianState state, core::GravitationalParameter mu) {
#ifdef ORBITFORGE_HAS_SPICE
    return providers::spice::to_keplerian(state, mu);
#else
    (void)state;
    (void)mu;
    throw std::runtime_error("Orbital element conversion requires ORBITFORGE_WITH_SPICE");
#endif
}
application::Session make_two_body_session(core::CartesianState initial,
                                           core::GravitationalParameter mu, std::string object_id,
                                           std::uint64_t input_revision) {
#ifdef ORBITFORGE_HAS_SPICE
    return {providers::spice::make_two_body(std::move(initial), mu),
            {std::move(object_id), input_revision, "two-body-cspice-n0067-prop2b-tai-v1"}};
#else
    (void)initial;
    (void)mu;
    (void)object_id;
    (void)input_revision;
    throw std::runtime_error("General two-body propagation requires ORBITFORGE_WITH_SPICE");
#endif
}
} // namespace orbitforge::runtime
