#include "../src/viewer/project_scene.hpp"
#include "../src/viewer/project_update.hpp"
#include <gtest/gtest.h>
#include <orbelle/runtime/gravity.hpp>
#include <orbelle/runtime/time_frames.hpp>

namespace orbelle::desktop {
namespace {
application::DataReference reference(std::string role, char content = 'a', char config = 'b') {
    return {std::move(role), "fixture", std::string(64, content), "fixture-v1",
            std::string(64, config)};
}
} // namespace

TEST(ProjectUpdate, ExplicitModelDependenciesDoNotRequestExternalDataConsent) {
    const application::DataLock before{{reference("earth-orientation"), reference("utc-time")}};
    const application::DataLock after{
        {reference("earth-orientation"), reference("utc-time"), reference("gravity/egm2008")}};
    EXPECT_TRUE(calculation_data_changes(before, after, true).empty());
    EXPECT_TRUE(calculation_data_changes(after, before, true).empty());
    EXPECT_EQ(calculation_data_changes(before, after, false).size(), 1);
    EXPECT_EQ(calculation_data_changes(after, before, false).size(), 1);
    const application::DataLock changed{
        {reference("earth-orientation", 'c'), reference("utc-time"), reference("gravity/egm2008")}};
    const auto changes = calculation_data_changes(after, changed, true);
    ASSERT_EQ(changes.size(), 1);
    EXPECT_EQ(changes[0].before->role, "earth-orientation");
    EXPECT_EQ(changes[0].after->content_sha256, std::string(64, 'c'));
    const application::DataLock policy{{reference("earth-orientation", 'a', 'd'),
                                        reference("utc-time"), reference("gravity/egm2008")}};
    EXPECT_EQ(calculation_data_changes(after, policy, true).size(), 1);
}

TEST(ProjectUpdate, SnapshotsPreserveEveryForceDependency) {
    const application::DataLock lock{{reference("earth-orientation"), reference("utc-time"),
                                      reference("gravity/egm2008"), reference("ephemeris/de440s"),
                                      reference("space-weather/celestrak"),
                                      reference("ocean-tides/fes")}};
    auto refs = force_data_references(lock);
    ASSERT_EQ(refs.size(), 4);
    refs.push_back(reference("earth-orientation"));
    refs.push_back(reference("utc-time"));
    EXPECT_EQ(application::DataLock{refs}, lock);
    const auto eop = runtime::load_eop_snapshot(ORBELLE_TEST_EOP);
    application::Project project{"all force references", eop->data_info().first_epoch,
                                 eop->data_info().last_epoch};
    project.earth_orientation = eop->data_info();
    project.force_data = force_data_references(lock);
    const auto reopened = application::decode_project(application::encode_project(project));
    EXPECT_EQ(reopened.data_lock(), project.data_lock());
    EXPECT_EQ(reopened.force_data, project.force_data);
}

TEST(ProjectUpdate, NumericalReuseTracksActiveInputsAndOnlyUsedResources) {
    application::OrbitInput input;
    input.propagator = application::OrbitPropagator::hpop;
    input.hpop.gravity_model = "egm2008";
    const application::DataLock data{{reference("earth-orientation"), reference("utc-time"),
                                      reference("gravity/egm2008"), reference("gravity/other")}};
    auto changed = input;
    changed.hpop.third_body(core::ThirdBody::sun).user_mu = 1e20; // Disabled draft.
    EXPECT_TRUE(can_reuse_orbit_session(input, data, changed, data));
    EXPECT_FALSE(same_orbit_input(input, changed));
    changed = input;
    changed.initial.velocity_mps[0] += 0.001;
    EXPECT_FALSE(can_reuse_orbit_session(input, data, changed, data));
    changed = input;
    changed.last = core::Epoch{input.last.tai_nanoseconds() + 1};
    EXPECT_FALSE(can_reuse_orbit_session(input, data, changed, data));
    changed = input;
    changed.step_seconds = 30;
    EXPECT_TRUE(can_reuse_orbit_session(input, data, changed, data));
    input.hpop.report_ephemeris_on_fixed_step = true;
    changed.hpop.report_ephemeris_on_fixed_step = true;
    EXPECT_FALSE(can_reuse_orbit_session(input, data, changed, data));
    const application::DataLock other{{reference("earth-orientation"), reference("utc-time"),
                                       reference("gravity/egm2008"),
                                       reference("gravity/other", 'c')}};
    EXPECT_TRUE(can_reuse_orbit_session(input, data, input, other));
    const application::DataLock own{{reference("earth-orientation"), reference("utc-time"),
                                     reference("gravity/egm2008", 'c'),
                                     reference("gravity/other")}};
    EXPECT_FALSE(can_reuse_orbit_session(input, data, input, own));
}

TEST(ProjectUpdate, TimeAndFrameDependenciesInvalidateOnlyRelevantProviders) {
    application::OrbitInput input;
    const application::DataLock data{{reference("earth-orientation"), reference("utc-time")}};
    const application::DataLock eop{{reference("earth-orientation", 'c'), reference("utc-time")}};
    const application::DataLock time{{reference("earth-orientation"), reference("utc-time", 'c')}};
    EXPECT_TRUE(can_reuse_orbit_session(input, data, input, eop));
    input.propagator = application::OrbitPropagator::sgp4;
    EXPECT_TRUE(can_reuse_orbit_session(input, data, input, eop));
    EXPECT_FALSE(can_reuse_orbit_session(input, data, input, time));
    input.propagator = application::OrbitPropagator::hpop;
    EXPECT_FALSE(can_reuse_orbit_session(input, data, input, eop));
}

TEST(ProjectUpdate, ResourceReuseAvoidsLoadsButChecksCompleteIdentityAndCancellation) {
    const auto model = core::builtin_egm96();
    struct File {
        std::string selection;
        core::GravityInfo info;
    };
    const std::vector<File> files{{"gravity", model->info()}};
    const std::map<std::string, std::shared_ptr<const core::GravityModel>> retained{
        {"gravity", model}};
    int loads = 0;
    bool cancel = false;
    const auto load = [&](const auto &) {
        ++loads;
        return model;
    };
    const auto info = [](const auto &value) -> const auto & { return value.info(); };
    const auto checkpoint = [&] {
        if (cancel)
            throw OrbitCancelled{};
    };
    auto result =
        prepare_resources(files, retained, runtime::gravity_reference, info, load, checkpoint);
    EXPECT_EQ(loads, 0);
    EXPECT_EQ(result.at("gravity"), model);
    result = prepare_resources(files, decltype(retained){}, runtime::gravity_reference, info, load,
                               checkpoint);
    EXPECT_EQ(loads, 1);
    auto changed = files;
    changed[0].info.solid_tide_k20 += .01; // Same content hash, different interpretation.
    EXPECT_THROW(
        prepare_resources(changed, retained, runtime::gravity_reference, info, load, checkpoint),
        std::runtime_error);
    EXPECT_EQ(loads, 2);
    EXPECT_EQ(retained.at("gravity"), model);
    cancel = true;
    EXPECT_THROW(
        prepare_resources(files, retained, runtime::gravity_reference, info, load, checkpoint),
        OrbitCancelled);
    EXPECT_EQ(loads, 2);
}

TEST(ProjectUpdate, EditingOneOfTenHpopSatellitesRetainsOtherNumericalSessions) {
    const std::shared_ptr<const core::ITimeProvider> time = runtime::make_builtin_time_provider();
    const std::shared_ptr<const core::IFrameProvider> frames = runtime::make_frame_provider();
    const std::shared_ptr<const core::IEarthOrientationProvider> eop =
        runtime::load_eop_snapshot(ORBELLE_TEST_EOP);
    const auto first = time->from_utc({2026, 10, 9, 0, 0, 0, 0});
    const core::Epoch last{first.tai_nanoseconds() + 3600'000'000'000LL};
    application::Project project{"ten satellites", first, last};
    for (int i = 0; i < 10; ++i) {
        application::ProjectSatellite satellite;
        satellite.orbit.object_id = "sat-" + std::to_string(i);
        satellite.orbit.propagator = application::OrbitPropagator::hpop;
        satellite.orbit.initial.epoch = first;
        satellite.orbit.initial.position_m[0] += i * 1000;
        satellite.orbit.first = first;
        satellite.orbit.last = last;
        project.satellites.push_back(satellite);
    }
    const std::atomic_bool cancelled{false};
    const auto before = prepare_project_scene(project, {}, frames, {eop}, cancelled, {}, {}, time);
    auto changed = before->project;
    changed.satellites[3].orbit.initial.velocity_mps[1] += 10;
    const auto after =
        prepare_project_scene(changed, {}, frames, {eop}, cancelled, before->scenario, "sat-3",
                              time, {}, {}, {}, {}, before->project);
    ASSERT_EQ(after->scenario.objects().size(), 10);
    for (std::size_t i = 0; i < 10; ++i) {
        EXPECT_EQ(after->scenario.objects()[i].provenance().input_revision, i == 3 ? 2 : 1);
        if (i != 3)
            EXPECT_EQ(
                after->scenario.objects()[i].sample(std::array{last}).states()[0].position_m,
                before->scenario.objects()[i].sample(std::array{last}).states()[0].position_m);
    }
    const auto unchanged =
        prepare_project_scene(after->project, {}, frames, {eop}, cancelled, after->scenario,
                              "sat-3", time, {}, {}, {}, {}, after->project);
    for (std::size_t i = 0; i < 10; ++i)
        EXPECT_EQ(unchanged->scenario.objects()[i].provenance().input_revision,
                  after->scenario.objects()[i].provenance().input_revision);
    EXPECT_EQ(before->scenario.objects()[3].provenance().input_revision, 1);
}
} // namespace orbelle::desktop
