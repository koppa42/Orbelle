#pragma once
#include <algorithm>
#include <map>
#include <orbitforge/application/project.hpp>
#include <string_view>

namespace orbitforge::desktop {
inline bool same_state(const core::CartesianState &a, const core::CartesianState &b) {
    return a.epoch == b.epoch && a.origin == b.origin && a.frame == b.frame &&
           a.position_m == b.position_m && a.velocity_mps == b.velocity_mps;
}

// Exact comparison of the saved draft, including inactive settings. Numerical reuse
// below has a separate key so changing an inactive draft never forces propagation.
inline bool same_orbit_input(const application::OrbitInput &a, const application::OrbitInput &b) {
    return same_state(a.initial, b.initial) && a.propagator == b.propagator &&
           a.input_frame == b.input_frame && a.tle == b.tle && a.hpop == b.hpop &&
           a.covariance == b.covariance && a.mission == b.mission && a.object_id == b.object_id &&
           a.first == b.first && a.last == b.last && a.step_seconds == b.step_seconds;
}

inline std::string orbit_calculation_key(const application::OrbitInput &input) {
    // Analytical providers are independent of the requested display interval.
    const bool bounded =
        input.uses_hpop() || input.propagator == application::OrbitPropagator::mission;
    application::Project numerical{"orbit-calculation", bounded ? input.first : core::Epoch{0},
                                   bounded ? input.last : core::Epoch{1}};
    application::ProjectSatellite satellite;
    satellite.orbit = input;
    numerical.satellites.push_back(std::move(satellite));
    return application::analysis_input_sha256(numerical);
}

inline application::DataLock orbit_calculation_data(const application::OrbitInput &input,
                                                    const application::DataLock &data) {
    std::vector<application::DataReference> refs;
    const auto hpop = input.hpop_resource_options();
    for (const auto &ref : data.references()) {
        bool used = ref.role == "earth-orientation" && input.requires_eop();
        used |= ref.role == "utc-time" &&
                (input.propagator == application::OrbitPropagator::sgp4 ||
                 (input.uses_hpop() && (hpop.uses_utc_force() || hpop.uses_ephemeris())));
        if (input.uses_hpop())
            used |=
                (!hpop.gravity_model.empty() && ref.role == "gravity/" + hpop.gravity_model) ||
                (hpop.uses_ephemeris() && ref.role == "ephemeris/" + hpop.ephemeris_model) ||
                (hpop.drag.enabled && !hpop.drag.weather_model.empty() &&
                 ref.role == "space-weather/" + hpop.drag.weather_model) ||
                (hpop.ocean_tides.active() && ref.role == "ocean-tides/" + hpop.ocean_tides.model);
        if (used)
            refs.push_back(ref);
    }
    return application::DataLock{std::move(refs)};
}

inline bool can_reuse_orbit_session(const application::OrbitInput &before,
                                    const application::DataLock &before_data,
                                    const application::OrbitInput &after,
                                    const application::DataLock &after_data) {
    return orbit_calculation_key(before) == orbit_calculation_key(after) &&
           orbit_calculation_data(before, before_data) == orbit_calculation_data(after, after_data);
}

inline std::vector<application::DataReference>
force_data_references(const application::DataLock &data) {
    std::vector<application::DataReference> refs;
    for (const auto &ref : data.references())
        if (ref.role != "earth-orientation" && ref.role != "utc-time")
            refs.push_back(ref);
    return refs;
}

struct DataChange {
    std::optional<application::DataReference> before, after;
};

inline std::vector<DataChange> calculation_data_changes(const application::DataLock &before,
                                                        const application::DataLock &after,
                                                        bool editing) {
    std::vector<DataChange> changes;
    if (before.references().empty())
        return changes; // First binding has no earlier calculation to replace.
    for (const auto &old : before.references()) {
        const auto found =
            std::ranges::find(after.references(), old.role, &application::DataReference::role);
        if (found != after.references().end()) {
            if (*found != old)
                changes.push_back({old, *found});
        } else if (!editing) {
            changes.push_back({old, {}});
        }
    }
    // During editing, adding/removing dependencies is part of the user's explicit
    // model/report change. Only replacement of an already used resource needs consent.
    if (!editing)
        for (const auto &ref : after.references())
            if (std::ranges::find(before.references(), ref.role,
                                  &application::DataReference::role) == before.references().end())
                changes.push_back({{}, ref});
    return changes;
}

// Reuse immutable providers only when their complete calculation identity matches.
// The candidate owns its maps; failure/cancellation cannot modify the active scene.
template <class Files, class Providers, class Reference, class Info, class Load, class Checkpoint>
Providers prepare_resources(const Files &files, const Providers &retained, Reference reference,
                            Info info, Load load, Checkpoint checkpoint) {
    Providers result;
    for (const auto &file : files) {
        checkpoint();
        const auto wanted = reference(file.selection, file.info);
        const auto found = retained.find(file.selection);
        if (found != retained.end() && found->second &&
            reference(file.selection, info(*found->second)) == wanted) {
            result.emplace(file.selection, found->second);
            continue;
        }
        auto model = load(file);
        checkpoint();
        if (reference(file.selection, info(*model)) != wanted)
            throw std::runtime_error("Selected data changed while preparing: " + file.selection);
        result.emplace(file.selection, std::move(model));
    }
    return result;
}
} // namespace orbitforge::desktop
