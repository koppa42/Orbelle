#pragma once
#include "startup_splash.hpp"
#include <map>
#include <orbelle/runtime/data_catalog.hpp>

namespace orbelle::desktop {
// Application-owned, immutable defaults, shared with scenes instead of copied.
// Keep at most one EGM2008 and one DE440s provider, never a history or all installed
// models. Active scenes/reports own their resources independently of this cache.
struct StartupResources {
    using Gravity = std::map<std::string, std::shared_ptr<const core::GravityModel>>;
    using Ephemerides = std::map<std::string, std::shared_ptr<const core::IThirdBodyEphemeris>>;
    Gravity gravity;
    Ephemerides ephemerides;
    void remember(const Gravity &used_gravity, const Ephemerides &used_ephemerides);
    // Drop obsolete cache ownership without changing any active computation.
    void reconcile(const std::vector<runtime::CatalogGravity> &installed_gravity,
                   const std::vector<runtime::CatalogEphemeris> &installed_ephemerides);
};
// Capture catalog records on the GUI thread; steps load sequentially on a worker.
// UTC/EOP already belong to EopCatalog. Other force resources remain demand-loaded.
std::vector<StartupSplash::Step>
startup_resource_steps(StartupResources &cache, const std::vector<runtime::CatalogGravity> &gravity,
                       const std::vector<runtime::CatalogEphemeris> &ephemerides);
} // namespace orbelle::desktop
