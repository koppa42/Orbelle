#pragma once
#include <orbitforge/core/propagation/hpop.hpp>
#include <orbitforge/runtime/data_catalog.hpp>
class QWidget;
namespace orbitforge::desktop {
// Edits private drafts; commits both settings only on acceptance.
bool edit_mission(core::MissionPlan &plan, core::HpopOptions &hpop, core::Epoch initial,
                  core::Epoch scenario_first, core::Epoch scenario_last, QWidget *parent = nullptr,
                  const std::vector<runtime::CatalogGravity> &models = {});
} // namespace orbitforge::desktop
