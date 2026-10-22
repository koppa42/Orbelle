#pragma once
#include <functional>
#include <orbelle/core/propagation/hpop.hpp>
#include <orbelle/runtime/data_catalog.hpp>
class QWidget;
namespace orbelle::desktop {
class EopCatalog;
// Edits private drafts; commits both settings only on acceptance.
bool edit_mission(
    core::MissionPlan &plan, core::HpopOptions &hpop, core::Epoch initial,
    core::Epoch scenario_first, core::Epoch scenario_last, QWidget *parent = nullptr,
    const std::vector<runtime::CatalogGravity> &models = {},
    const std::vector<runtime::CatalogEphemeris> &ephemerides = {},
    const std::function<std::vector<runtime::CatalogEphemeris>()> &manage_ephemerides = {},
    EopCatalog *catalog = nullptr, bool initialize_hpop = false);
} // namespace orbelle::desktop
