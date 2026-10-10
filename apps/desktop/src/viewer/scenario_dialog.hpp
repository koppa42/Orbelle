#pragma once
#include <functional>
#include <optional>
#include <orbitforge/application/scenario.hpp>
class QWidget;
namespace orbitforge::desktop {
struct ScenarioSettings {
    std::string name;
    core::Epoch first, last;
};
[[nodiscard]] std::optional<application::Scenario>
new_scenario_dialog(QWidget *parent, core::Epoch coverage_first, core::Epoch coverage_last,
                    const std::function<void(core::Epoch, core::Epoch)> &validate);
} // namespace orbitforge::desktop
