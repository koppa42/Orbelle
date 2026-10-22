#pragma once
#include <orbelle/runtime/access.hpp>

namespace orbelle::runtime::detail {
// Shared constraints for legacy reports and fixed Analysis queries. No propagation or I/O.
struct AccessGeometry {
    application::ProjectGroundStation station;
    application::ProjectSatellite satellite;
    std::optional<application::ProjectSensor> sensor;
    std::optional<core::SensorGeometry> pattern;
    std::optional<application::ProjectGroundStation> pointing_target;

    AccessGeometry(const application::Project &project, const std::vector<std::string> &ids);
    [[nodiscard]] AccessSample at(const core::CartesianState &itrf) const;
    [[nodiscard]] std::string target_name() const;
    [[nodiscard]] application::Project numerical_inputs(core::Epoch first, core::Epoch last) const;
};
} // namespace orbelle::runtime::detail
