#pragma once
#include <orbitforge/core/propagation/hpop.hpp>
#include <orbitforge/runtime/data_catalog.hpp>
class QWidget;
namespace orbitforge::desktop {
// Commits only when the user accepts valid settings. Cancel leaves the draft intact.
bool edit_hpop_settings(core::HpopOptions &options, QWidget *parent,
                        const std::vector<runtime::CatalogGravity> &models = {});
} // namespace orbitforge::desktop
