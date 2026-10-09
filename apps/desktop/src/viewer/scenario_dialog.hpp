#pragma once
#include <functional>
#include <optional>
#include <orbitforge/application/scenario.hpp>
class QWidget;
namespace orbitforge::desktop {
[[nodiscard]] std::optional<application::Scenario>
new_scenario_dialog(QWidget *parent, core::Epoch coverage_first, core::Epoch coverage_last,
                    const std::function<void(core::Epoch, core::Epoch)> &validate);
}
