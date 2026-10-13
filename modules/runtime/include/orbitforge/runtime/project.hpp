#pragma once
#include <orbitforge/application/project.hpp>
#include <orbitforge/application/scenario.hpp>

namespace orbitforge::runtime {
[[nodiscard]] application::Session
make_ground_station_session(const application::ProjectGroundStation &station,
                            std::uint64_t revision = 1);
[[nodiscard]] application::Session make_orbit_session(const application::OrbitInput &input,
                                                      std::uint64_t revision = 1,
                                                      const core::ITimeProvider *time = nullptr,
                                                      core::HpopEnvironment environment = {});
// Rebuilds a new immutable computation snapshot; no display cache is stored on disk.
// SGP4 requires time; HPOP spherical gravity requires captured frames and EOP.
[[nodiscard]] application::Scenario make_project_scenario(const application::Project &project,
                                                          const core::ITimeProvider *time = nullptr,
                                                          core::HpopEnvironment environment = {});
} // namespace orbitforge::runtime
