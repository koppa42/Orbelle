#include <orbelle/runtime/j4.hpp>
#ifdef ORBELLE_HAS_SPICE
#include <orbelle/providers/spice/provider.hpp>
#endif

namespace orbelle::runtime {
core::CartesianState j4_mean_ellipse_state(core::J4MeanElements elements) {
#ifdef ORBELLE_HAS_SPICE
    return providers::spice::j2_mean_ellipse_state(elements, core::j4_earth());
#else
    (void)elements;
    throw std::runtime_error("J4 propagation requires ORBELLE_WITH_SPICE");
#endif
}
core::J4MeanElements j4_mean_ellipse_elements(core::CartesianState auxiliary_state) {
#ifdef ORBELLE_HAS_SPICE
    return providers::spice::j2_mean_ellipse_elements(auxiliary_state, core::j4_earth());
#else
    (void)auxiliary_state;
    throw std::runtime_error("J4 propagation requires ORBELLE_WITH_SPICE");
#endif
}
application::Session make_j4_session(core::J4MeanElements initial, std::string object_id,
                                     std::uint64_t input_revision) {
#ifdef ORBELLE_HAS_SPICE
    return {providers::spice::make_j4(initial),
            {std::move(object_id), input_revision,
             "j4-secular-kozai-gcrf-derivative-v1;earth-egm2008-j4-v1"}};
#else
    (void)initial;
    (void)object_id;
    (void)input_revision;
    throw std::runtime_error("J4 propagation requires ORBELLE_WITH_SPICE");
#endif
}
} // namespace orbelle::runtime
