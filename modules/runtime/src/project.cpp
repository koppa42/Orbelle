#include <orbitforge/runtime/j2.hpp>
#include <orbitforge/runtime/project.hpp>

namespace orbitforge::runtime {
application::Session make_orbit_session(const application::OrbitInput &input,
                                        std::uint64_t revision) {
    const auto &body = core::central_body(input.initial.origin);
    switch (input.propagator) {
    case application::OrbitPropagator::two_body:
        return make_two_body_session(input.initial, body.mu, input.object_id, revision);
    case application::OrbitPropagator::j2_secular:
        return make_j2_session(j2_mean_ellipse_elements(input.initial), core::J2SecularOptions{},
                               input.object_id, revision, body.origin);
    }
    throw std::invalid_argument("Unsupported orbit propagator");
}
application::Scenario make_project_scenario(const application::Project &project) {
    application::validate_project(project);
    application::Scenario result{project.name, project.first, project.last};
    result = result.with_data_lock(project.data_lock());
    for (const auto &satellite : project.satellites)
        result = result.with_object(make_orbit_session(satellite.orbit));
    return result;
}
} // namespace orbitforge::runtime
