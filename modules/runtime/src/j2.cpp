#include <orbelle/runtime/j2.hpp>
#ifdef ORBELLE_HAS_SPICE
#include <orbelle/providers/spice/provider.hpp>
#endif
#include <stdexcept>

namespace orbelle::runtime {
core::CartesianState j2_mean_ellipse_state(core::J2MeanElements elements, core::OriginId body) {
#ifdef ORBELLE_HAS_SPICE
    return providers::spice::j2_mean_ellipse_state(elements, core::central_body(body));
#else
    (void)elements;
    (void)body;
    throw std::runtime_error("J2 propagation requires ORBELLE_WITH_SPICE");
#endif
}
core::J2MeanElements j2_mean_ellipse_elements(core::CartesianState auxiliary_state) {
#ifdef ORBELLE_HAS_SPICE
    return providers::spice::j2_mean_ellipse_elements(auxiliary_state,
                                                      core::central_body(auxiliary_state.origin));
#else
    (void)auxiliary_state;
    throw std::runtime_error("J2 propagation requires ORBELLE_WITH_SPICE");
#endif
}
application::Session make_j2_session(core::J2MeanElements initial, core::J2SecularOptions options,
                                     std::string object_id, std::uint64_t input_revision,
                                     core::OriginId body) {
#ifdef ORBELLE_HAS_SPICE
    const auto &gravity = core::central_body(body);
    const auto algorithm = std::string("j2-secular-gcrf-n0-derivative-v1;") +
                           std::string(gravity.model) +
                           ";node=" + (options.node_precession ? "1" : "0") +
                           ";apsis=" + (options.apsidal_precession ? "1" : "0") +
                           ";mean=" + (options.mean_motion_correction ? "1" : "0");
    return {providers::spice::make_j2(initial, gravity, options),
            {std::move(object_id), input_revision, algorithm}};
#else
    (void)initial;
    (void)options;
    (void)object_id;
    (void)input_revision;
    (void)body;
    throw std::runtime_error("J2 propagation requires ORBELLE_WITH_SPICE");
#endif
}
} // namespace orbelle::runtime
