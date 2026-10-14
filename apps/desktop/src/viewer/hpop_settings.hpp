#pragma once
#include <orbitforge/core/propagation/hpop.hpp>
class QWidget;
namespace orbitforge::desktop {
// Commits only when the user accepts valid settings. Cancel leaves the draft intact.
bool edit_hpop_settings(core::HpopOptions &options, QWidget *parent);
} // namespace orbitforge::desktop
