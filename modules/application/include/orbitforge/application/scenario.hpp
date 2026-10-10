#pragma once
#include <orbitforge/application/data_lock.hpp>
#include <orbitforge/application/session.hpp>
#include <string>
#include <vector>

namespace orbitforge::application {
// Immutable scene input. A new scenario has no objects. All registered sessions
// share its analysis interval; editing returns a new value, never mutating readers.
class Scenario {
  public:
    Scenario(std::string name, core::Epoch first, core::Epoch last);
    [[nodiscard]] const std::string &name() const noexcept { return name_; }
    [[nodiscard]] core::Epoch first() const noexcept { return first_; }
    [[nodiscard]] core::Epoch last() const noexcept { return last_; }
    [[nodiscard]] std::uint64_t revision() const noexcept { return revision_; }
    [[nodiscard]] std::span<const Session> objects() const noexcept { return objects_; }
    // Insert a new ID at revision 1, or replace its immediately preceding revision.
    // This rejects duplicate insertions and stale asynchronous candidates.
    [[nodiscard]] Scenario with_object(Session session) const;
    [[nodiscard]] Scenario without_object(const std::string &object_id) const;
    // Analysis bounds and display name do not change numerical initial conditions.
    // Callers must validate external data coverage before publishing the new scene.
    [[nodiscard]] Scenario with_settings(std::string name, core::Epoch first,
                                         core::Epoch last) const;
    [[nodiscard]] const DataLock &data_lock() const noexcept { return data_lock_; }
    [[nodiscard]] Scenario with_data_lock(DataLock lock) const;
    [[nodiscard]] std::vector<results::StateSeries>
    sample(std::span<const core::Epoch> epochs) const;

  private:
    std::string name_;
    core::Epoch first_, last_;
    std::uint64_t revision_ = 1;
    std::vector<Session> objects_;
    DataLock data_lock_;
};
} // namespace orbitforge::application
