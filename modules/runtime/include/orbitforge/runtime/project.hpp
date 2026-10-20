#pragma once
#include <memory>
#include <orbitforge/application/project.hpp>
#include <orbitforge/application/scenario.hpp>
#include <span>

namespace orbitforge::runtime {
// Effective orbital inputs and actually used resource identities, independent of
// display settings. Bounded HPOP/mission solutions include their integration interval.
[[nodiscard]] std::string orbit_calculation_key(const application::OrbitInput &input);
[[nodiscard]] application::DataLock orbit_calculation_data(const application::OrbitInput &input,
                                                           const application::DataLock &data);

// C++ adapter for sharing scientific sessions between a view and an Analysis.
// One entry per object; immutable providers support concurrent reads. Fork before
// preparing edits, and publish the fork only after the whole update succeeds.
class OrbitSessionCache {
  public:
    OrbitSessionCache();
    OrbitSessionCache(const OrbitSessionCache &) = delete;
    OrbitSessionCache &operator=(const OrbitSessionCache &) = delete;
    [[nodiscard]] std::shared_ptr<OrbitSessionCache> fork() const;
    void retain_objects(std::span<const std::string> ids);
    [[nodiscard]] application::Session acquire(const application::OrbitInput &input,
                                               std::uint64_t revision,
                                               const core::ITimeProvider *time = nullptr,
                                               core::HpopEnvironment environment = {});

  private:
    struct Impl;
    std::shared_ptr<Impl> impl_;
};

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
