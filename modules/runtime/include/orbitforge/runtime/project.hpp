#pragma once
#include <orbitforge/application/project.hpp>
#include <orbitforge/application/scenario.hpp>

namespace orbitforge::runtime {
[[nodiscard]] application::Session
make_ground_station_session(const application::ProjectGroundStation &station,
                            std::uint64_t revision = 1);
[[nodiscard]] application::Session make_orbit_session(const application::OrbitInput &input,
                                                      std::uint64_t revision = 1,
                                                      const core::ITimeProvider *time = nullptr);
// Rebuilds a new immutable computation snapshot; no display cache is stored on disk.
// time is required for SGP4 objects; other propagators do not need UTC data.
[[nodiscard]] application::Scenario
make_project_scenario(const application::Project &project,
                      const core::ITimeProvider *time = nullptr);
} // namespace orbitforge::runtime
