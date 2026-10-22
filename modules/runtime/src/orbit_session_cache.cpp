#include <algorithm>
#include <map>
#include <mutex>
#include <orbelle/runtime/ephemeris.hpp>
#include <orbelle/runtime/gravity.hpp>
#include <orbelle/runtime/ocean_tides.hpp>
#include <orbelle/runtime/project.hpp>
#include <orbelle/runtime/space_weather.hpp>
#include <set>

namespace orbelle::runtime {
std::string orbit_calculation_key(const application::OrbitInput &input) {
    const bool bounded =
        input.uses_hpop() || input.propagator == application::OrbitPropagator::mission;
    application::Project numerical{"orbit-calculation", bounded ? input.first : core::Epoch{0},
                                   bounded ? input.last : core::Epoch{1}};
    application::ProjectSatellite satellite;
    satellite.orbit = input;
    numerical.satellites.push_back(std::move(satellite));
    auto key = application::analysis_input_sha256(numerical);
    // Mission providers also own the names returned by maneuvers(). A renamed
    // segment must not expose a previously cached label through the new analysis.
    if (input.propagator == application::OrbitPropagator::mission)
        for (const auto &segment : input.mission.segments)
            key += ":" + std::to_string(segment.name.size()) + ":" + segment.name;
    return key;
}
application::DataLock orbit_calculation_data(const application::OrbitInput &input,
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
namespace {
application::DataLock resource_identity(const application::OrbitInput &input,
                                        const core::ITimeProvider *time,
                                        const core::HpopEnvironment &environment) {
    std::vector<application::DataReference> refs;
    if (environment.eop) {
        const auto eop = application::eop_data_lock(environment.eop->data_info());
        refs.assign(eop.references().begin(), eop.references().end());
    }
    std::erase_if(refs, [](const auto &ref) { return ref.role == "utc-time"; });
    const auto *clock =
        input.propagator == application::OrbitPropagator::sgp4 ? time : environment.time.get();
    if (clock) {
        const auto utc = application::time_data_lock(clock->data_info());
        refs.insert(refs.end(), utc.references().begin(), utc.references().end());
    }
    const auto hpop = input.hpop_resource_options();
    if (input.uses_hpop()) {
        if (!hpop.gravity_model.empty() && environment.gravity)
            refs.push_back(gravity_reference(hpop.gravity_model, environment.gravity->info()));
        if (hpop.uses_ephemeris() && environment.ephemeris)
            refs.push_back(
                ephemeris_reference(hpop.ephemeris_model, environment.ephemeris->data_info()));
        if (hpop.drag.enabled && !hpop.drag.weather_model.empty() && environment.weather)
            refs.push_back(
                space_weather_reference(hpop.drag.weather_model, environment.weather->info()));
        if (hpop.ocean_tides.active() && environment.ocean_tides)
            refs.push_back(
                ocean_tide_reference(hpop.ocean_tides.model, environment.ocean_tides->info()));
    }
    return orbit_calculation_data(input, application::DataLock{std::move(refs)});
}
} // namespace
struct OrbitSessionCache::Impl {
    struct Entry {
        std::string key;
        application::DataLock data;
        // A different frame implementation may produce different rotations despite
        // identical data bytes. Retain and compare the selected implementation too.
        std::shared_ptr<const core::IFrameProvider> frames;
        application::Session session;
    };
    mutable std::mutex mutex;
    std::map<std::string, Entry> entries;
};
OrbitSessionCache::OrbitSessionCache() : impl_(std::make_shared<Impl>()) {}
std::shared_ptr<OrbitSessionCache> OrbitSessionCache::fork() const {
    auto result = std::make_shared<OrbitSessionCache>();
    const std::lock_guard lock(impl_->mutex);
    result->impl_->entries = impl_->entries;
    return result;
}
void OrbitSessionCache::retain_objects(std::span<const std::string> ids) {
    const std::set<std::string> retained(ids.begin(), ids.end());
    const std::lock_guard lock(impl_->mutex);
    std::erase_if(impl_->entries, [&](const auto &item) { return !retained.contains(item.first); });
}
application::Session OrbitSessionCache::acquire(const application::OrbitInput &input,
                                                std::uint64_t revision,
                                                const core::ITimeProvider *time,
                                                core::HpopEnvironment environment) {
    if (input.uses_hpop()) {
        application::validate_hpop_covariance_settings(input.covariance, input.hpop);
        if (input.covariance.enabled && !input.computes_covariance())
            throw std::invalid_argument("Covariance across mission maneuvers is not supported");
    }
    const auto key = orbit_calculation_key(input);
    const auto data = resource_identity(input, time, environment);
    const auto frames = input.requires_eop() ? environment.frames : nullptr;
    const std::lock_guard lock(impl_->mutex);
    const auto found = impl_->entries.find(input.object_id);
    if (found != impl_->entries.end() && found->second.key == key && found->second.data == data &&
        found->second.frames == frames)
        return found->second.session.with_input_revision(revision);
    // Session construction validates the input/resources but does not sample its
    // lazy ephemeris. A failed construction leaves the previous entry unchanged.
    auto session = make_orbit_session(input, revision, time, std::move(environment));
    impl_->entries.insert_or_assign(input.object_id, Impl::Entry{key, data, frames, session});
    return session;
}
} // namespace orbelle::runtime
