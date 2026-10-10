#pragma once
#include <functional>
#include <memory>
#include <optional>
#include <orbitforge/application/scenario.hpp>
#include <orbitforge/core/time/time_provider.hpp>
class QWidget;
namespace orbitforge::desktop {
struct ScenarioSettings {
    std::string name;
    core::Epoch first, last;
};
[[nodiscard]] std::optional<application::Scenario>
new_scenario_dialog(QWidget *parent, core::Epoch coverage_first, core::Epoch coverage_last,
                    const std::function<void(core::Epoch, core::Epoch)> &validate,
                    std::shared_ptr<const core::ITimeProvider> time = {});
} // namespace orbitforge::desktop
