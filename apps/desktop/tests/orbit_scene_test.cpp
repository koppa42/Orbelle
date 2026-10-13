#include "../src/viewer/display_settings.hpp"
#include <chrono>
#include <gtest/gtest.h>
#include <orbitforge/runtime/j2.hpp>
#include <orbitforge/runtime/time_frames.hpp>
#include <orbitforge/runtime/two_body.hpp>

namespace orbitforge::desktop {
TEST(OrbitPreparation, MissionBurnsKeepSeparateTangentsOnAndOffTheDisplayGrid) {
    const auto time = runtime::make_builtin_time_provider();
    const auto frames = runtime::make_frame_provider();
    const auto eop = runtime::load_eop_snapshot(ORBITFORGE_TEST_EOP);
    for (auto offset : {300'000'000'000LL, 301'234'567'890LL}) {
        OrbitInput input;
        input.first = time->from_utc({2026, 10, 9, 0, 0, 0, 0});
        input.last = core::Epoch{input.first.tai_nanoseconds() + 1200'000'000'000LL};
        input.initial.epoch = input.first;
        input.propagator = OrbitPropagator::mission;
        input.mission.segments = {
            core::MissionSegment::coast(std::chrono::nanoseconds{offset}),
            core::MissionSegment::impulse({250, 20, -30}),
            core::MissionSegment::coast(std::chrono::nanoseconds{1200'000'000'000LL - offset})};
        const auto session = make_orbit_session(input, 1);
        const auto orbit =
            prepare_orbit_scene(input, session, *frames, *eop, std::atomic_bool{false});
        EXPECT_LT(orbit->max_inertial_check_error_m, 1);
        EXPECT_LT(orbit->max_check_error_m, 1);
        const auto &nodes = orbit->inertial_track.samples().states();
        const auto burn = input.first.tai_nanoseconds() + offset;
        for (auto at : {burn - 1, burn}) {
            const auto found = std::ranges::find_if(
                nodes, [at](const auto &state) { return state.epoch.tai_nanoseconds() == at; });
            ASSERT_NE(found, nodes.end());
            EXPECT_EQ(found->velocity_mps,
                      session.sample(std::array{core::Epoch{at}}).states()[0].velocity_mps);
        }
    }
}
TEST(OrbitPreparation, J4Sgp4AndHpopBuildGcrfAndItrfCachesFromTheirNativeModels) {
    const auto time = runtime::make_builtin_time_provider();
    const std::shared_ptr<const core::IFrameProvider> frames = runtime::make_frame_provider();
    const std::shared_ptr<const core::IEarthOrientationProvider> eop =
        runtime::load_eop_snapshot(ORBITFORGE_TEST_EOP);
    for (auto model : {OrbitPropagator::j4_secular, OrbitPropagator::sgp4, OrbitPropagator::hpop}) {
        OrbitInput input;
        input.first = time->from_utc({2026, 10, 9, 0, 0, 0, 0});
        input.last = time->from_utc({2026, 10, 10, 0, 0, 0, 0});
        input.initial.epoch = input.first;
        input.propagator = model;
        if (model == OrbitPropagator::sgp4)
            input.tle = {"1 00005U 58002B   26282.00000000  .00000023  00000-0  28098-4 0  4755",
                         "2 00005  34.2682 348.7242 1859667 331.7664  19.3264 10.82419157413667"};
        const auto session = make_orbit_session(input, 1, time.get(), {frames, eop});
        const auto orbit =
            prepare_orbit_scene(input, session, *frames, *eop, std::atomic_bool{false},
                                display_cache_interval(input, {}, input.first));
        EXPECT_EQ(orbit->inertial_track.samples().states()[0].frame, core::FrameId::gcrf);
        EXPECT_EQ(orbit->track.samples().states()[0].frame, core::FrameId::itrf);
        EXPECT_LE(orbit->max_check_error_m, 1);
        EXPECT_LE(orbit->max_inertial_check_error_m, 1);
        const auto native = session.sample(std::array{input.first}).states()[0];
        const auto converted =
            core::transform_state(native, core::FrameId::gcrf, *frames, eop->at(input.first));
        EXPECT_EQ(converted.position_m, orbit->inertial_track.samples().states()[0].position_m);
    }
}
TEST(OrbitPreparation, J2MinuteCacheUsesAllSecularTermsAndTrajectoryDerivative) {
    const auto time = runtime::make_builtin_time_provider();
    const auto frames = runtime::make_frame_provider();
    const auto eop = runtime::load_eop_snapshot(ORBITFORGE_TEST_EOP);
    OrbitInput input;
    input.first = time->from_utc({2026, 10, 9, 0, 0, 0, 0});
    input.last = time->from_utc({2026, 10, 10, 0, 0, 0, 0});
    input.initial.epoch = input.first;
    input.propagator = OrbitPropagator::j2_secular;
    const auto session = make_orbit_session(input, 3);
    EXPECT_NE(session.provenance().algorithm.find("node=1;apsis=1;mean=1"), std::string::npos);
    const std::atomic_bool cancelled{false};
    for (const auto epoch :
         {input.first, core::Epoch{input.first.tai_nanoseconds() + 12 * 3600'000'000'000LL}}) {
        const auto orbit = prepare_orbit_scene(input, session, *frames, *eop, cancelled,
                                               display_cache_interval(input, {}, epoch));
        EXPECT_LE(orbit->max_check_error_m, 1);
        EXPECT_LE(orbit->max_inertial_check_error_m, 1);
        EXPECT_EQ(orbit->session.provenance().algorithm, session.provenance().algorithm);
        EXPECT_LT(orbit->track.samples().states().size(), 200);
    }
}
TEST(OrbitPreparation, FullDayRetainsValidatedEarthFixedCache) {
    const auto time = runtime::make_builtin_time_provider();
    const auto frames = runtime::make_frame_provider();
    const auto eop = runtime::load_eop_snapshot(ORBITFORGE_TEST_EOP);
    OrbitInput input;
    input.step_seconds = 10; // Fixed input for the before/after full-day timing comparison.
    input.first = time->from_utc({2026, 10, 9, 0, 0, 0, 0});
    input.last = time->from_utc({2026, 10, 10, 0, 0, 0, 0});
    input.initial.epoch = input.first;
    auto session = runtime::make_two_body_session(
        input.initial, core::GravitationalParameter{input.mu()}, input.object_id, 1);
    const std::atomic_bool cancelled{false};
    const auto start = std::chrono::steady_clock::now();
    const auto orbit = prepare_orbit_scene(input, std::move(session), *frames, *eop, cancelled);
    const auto seconds =
        std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();
    RecordProperty("preparation_seconds", seconds);
    RecordProperty("max_itrf_check_error_m", orbit->max_check_error_m);
    EXPECT_EQ(orbit->track.samples().states().size(), 8641);
    EXPECT_LE(orbit->max_check_error_m, 1.0);
    EXPECT_LE(orbit->max_inertial_check_error_m, 1.0);
    EXPECT_EQ(orbit->inertial_track.samples().states().size(), 8641);
    EXPECT_EQ(orbit->track.sample(input.first).frame, core::FrameId::itrf);
    EXPECT_THROW((void)orbit->track.sample(core::Epoch{input.first.tai_nanoseconds() - 1}),
                 std::out_of_range);
}
TEST(OrbitPreparation, DefaultMinuteStepPreparesOnlyTheRequestedPeriod) {
    const auto time = runtime::make_builtin_time_provider();
    const auto frames = runtime::make_frame_provider();
    const auto eop = runtime::load_eop_snapshot(ORBITFORGE_TEST_EOP);
    OrbitInput input;
    ASSERT_EQ(input.step_seconds, 60);
    input.first = time->from_utc({2026, 10, 9, 0, 0, 0, 0});
    input.last = time->from_utc({2026, 10, 10, 0, 0, 0, 0});
    input.initial.epoch = input.first;
    const auto session = runtime::make_two_body_session(
        input.initial, core::GravitationalParameter{input.mu()}, input.object_id, 7);
    const std::atomic_bool cancelled{false};
    const DisplaySettings settings;
    const auto orbit = prepare_orbit_scene(input, session, *frames, *eop, cancelled,
                                           display_cache_interval(input, settings, input.first));
    RecordProperty("preparation_seconds", orbit->preparation_seconds);
    RecordProperty("cache_nodes", static_cast<int>(orbit->track.samples().states().size()));
    RecordProperty("max_itrf_check_error_m", orbit->max_check_error_m);
    RecordProperty("max_inertial_check_error_m", orbit->max_inertial_check_error_m);
    EXPECT_LT(orbit->track.samples().states().size(), 200);
    EXPECT_GT(orbit->track.samples().states().size(), 2);
    EXPECT_EQ(orbit->input.last, input.last);
    EXPECT_EQ(orbit->session.provenance().input_revision, 7);
    const auto wanted = display_interval(input, settings, input.first);
    EXPECT_TRUE(cache_covers(*orbit, wanted.first, wanted.second));
    const core::Epoch jump{input.first.tai_nanoseconds() + 12 * 3600'000'000'000LL};
    EXPECT_FALSE(cache_covers(*orbit, jump, jump));
    // The analysis session still supplies an immediate exact state outside the display cache.
    EXPECT_EQ(orbit->session.sample(std::array{jump}).states().front().epoch, jump);
    EXPECT_THROW((void)orbit->track.sample(jump), std::out_of_range);
    const auto moved = prepare_orbit_scene(input, orbit->session, *frames, *eop, cancelled,
                                           display_cache_interval(input, settings, jump));
    const auto moved_wanted = display_interval(input, settings, jump);
    EXPECT_TRUE(cache_covers(*moved, moved_wanted.first, moved_wanted.second));
    EXPECT_FALSE(cache_covers(*moved, input.first, input.first));
    EXPECT_EQ(moved->session.provenance().input_revision, 7);
    EXPECT_LE(moved->max_check_error_m, 1);
    EXPECT_LE(moved->max_inertial_check_error_m, 1);
    const auto r =
        frames->gcrf_to_itrf(jump, eop->at(jump).sample)
            .apply_position(moved->inertial_track.sample(jump).markers.front().position_m);
    const auto fixed = moved->track.sample(jump).markers.front().position_m;
    EXPECT_LE(std::hypot(r[0] - fixed[0], r[1] - fixed[1], r[2] - fixed[2]), 2);
}
TEST(DisplayInterval, FullCustomAndOpenOrbitWindowsStayWithinTheScenario) {
    OrbitInput input;
    input.last = core::Epoch{86400'000'000'000LL};
    const DisplaySettings full{.extent = 1};
    EXPECT_EQ(display_interval(input, full, input.first), std::make_pair(input.first, input.last));
    const DisplaySettings trailing{.extent = 2, .past_minutes = 30, .future_minutes = 0};
    const auto empty = display_interval(input, trailing, input.first);
    EXPECT_EQ(empty.first, empty.second);
    const auto seed = display_cache_interval(input, trailing, input.first);
    EXPECT_EQ(seed.first, input.first);
    EXPECT_GT(seed.second.tai_nanoseconds(), seed.first.tai_nanoseconds());
    EXPECT_LE(seed.second.tai_nanoseconds(), 60'000'000'000);
    const auto last = display_cache_interval(input, DisplaySettings{}, input.last);
    EXPECT_EQ(last.second, input.last);
    EXPECT_GT(last.first.tai_nanoseconds(), input.first.tai_nanoseconds());
    input.initial.velocity_mps = {0, 15000, 0}; // Hyperbolic; no invented orbital period.
    const auto open = display_interval(input, DisplaySettings{}, input.first);
    EXPECT_EQ(open.second.tai_nanoseconds() - open.first.tai_nanoseconds(), 3600'000'000'000LL);
    EXPECT_THROW((void)display_interval(
                     input, {.extent = 2, .past_minutes = 0, .future_minutes = 0}, input.first),
                 std::invalid_argument);
}
} // namespace orbitforge::desktop
