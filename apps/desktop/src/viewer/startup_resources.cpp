#include "startup_resources.hpp"
#include "../ui_text.hpp"
#include "project_update.hpp"
#include <orbitforge/runtime/ephemeris.hpp>
#include <orbitforge/runtime/gravity.hpp>

namespace orbitforge::desktop {
namespace {
template <class Models> void remember_default(Models &cache, const Models &used, const char *id) {
    if (const auto it = used.find(id); it != used.end() && it->second)
        cache = {{id, it->second}};
}
template <class Models, class Files, class Reference, class Info>
void reconcile_default(Models &cache, const Files &files, Reference reference, Info info) {
    std::erase_if(cache, [&](const auto &entry) {
        return std::none_of(files.begin(), files.end(), [&](const auto &file) {
            return file.selection == entry.first && entry.second &&
                   reference(file.selection, file.info) ==
                       reference(entry.first, info(*entry.second));
        });
    });
}
} // namespace
void StartupResources::remember(const Gravity &used_gravity, const Ephemerides &used_ephemerides) {
    remember_default(gravity, used_gravity, "egm2008");
    remember_default(ephemerides, used_ephemerides, "de440s");
}
void StartupResources::reconcile(
    const std::vector<runtime::CatalogGravity> &installed_gravity,
    const std::vector<runtime::CatalogEphemeris> &installed_ephemerides) {
    reconcile_default(gravity, installed_gravity, runtime::gravity_reference,
                      [](const auto &model) -> const auto & { return model.info(); });
    reconcile_default(ephemerides, installed_ephemerides, runtime::ephemeris_reference,
                      [](const auto &model) -> const auto & { return model.data_info(); });
}
std::vector<StartupSplash::Step>
startup_resource_steps(StartupResources &cache, const std::vector<runtime::CatalogGravity> &gravity,
                       const std::vector<runtime::CatalogEphemeris> &ephemerides) {
    cache.reconcile(gravity, ephemerides);
    std::vector<StartupSplash::Step> steps;
    for (const auto &file : gravity)
        if (file.selection == "egm2008")
            steps.push_back(
                {ui::text("startup_splash.gravity"), [&cache, file](const auto &checkpoint) {
                     cache.gravity = prepare_resources(
                         std::vector{file}, cache.gravity, runtime::gravity_reference,
                         [](const auto &model) -> const auto & { return model.info(); },
                         [&](const auto &record) {
                             return runtime::load_gravity(record.file, record.info.source_url,
                                                          checkpoint);
                         },
                         checkpoint);
                 }});
    for (const auto &file : ephemerides)
        if (runtime::ephemeris_available() && file.selection == "de440s")
            steps.push_back(
                {ui::text("startup_splash.ephemeris"), [&cache, file](const auto &checkpoint) {
                     cache.ephemerides = prepare_resources(
                         std::vector{file}, cache.ephemerides, runtime::ephemeris_reference,
                         [](const auto &model) -> const auto & { return model.data_info(); },
                         [&](const auto &record) {
                             return runtime::make_third_body_ephemeris(runtime::load_spice_snapshot(
                                 record.manifest,
                                 record.manifest.parent_path().parent_path().parent_path() /
                                     "runtime"));
                         },
                         checkpoint);
                 }});
    return steps;
}
} // namespace orbitforge::desktop
