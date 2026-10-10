#pragma once
#include <orbitforge/application/project.hpp>
#include <orbitforge/application/scenario.hpp>

namespace orbitforge::runtime {
[[nodiscard]] application::Session make_orbit_session(const application::OrbitInput &input,
                                                      std::uint64_t revision = 1);
// Rebuilds a new immutable computation snapshot; no display cache is stored on disk.
[[nodiscard]] application::Scenario make_project_scenario(const application::Project &project);
} // namespace orbitforge::runtime
