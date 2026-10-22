#include <gtest/gtest.h>
#include <limits>
#include <orbelle/runtime/analysis.hpp>
#include <orbelle/runtime/project.hpp>
#include <orbelle/runtime/time_frames.hpp>
#include <orbelle/runtime/two_body.hpp>

using namespace orbelle;
using namespace std::chrono_literals;
namespace app = orbelle::application;
namespace {
app::ProjectDocument project(core::Epoch first = core::Epoch{0},
                             core::Epoch last = core::Epoch{10}) {
    return app::ProjectDocument{app::Project{"Analysis", first, last}};
}
auto station(app::ProjectDocument &document) {
    return document.objects().create(app::objects::GroundStation{"Equator", {}, 0});
}
auto epochs(const results::Trajectory &result) {
    std::vector<std::int64_t> ticks;
    for (const auto &s : result.states())
        ticks.push_back(s.epoch.tai_nanoseconds());
    return ticks;
}
class MismatchedEop final : public core::IEarthOrientationProvider {
  public:
    explicit MismatchedEop(std::shared_ptr<const core::IEarthOrientationProvider> source)
        : source_(std::move(source)), info_(source_->data_info()) {
        info_.time_data.sha256 = std::string(64, 'a');
    }
    core::EarthOrientationState at(core::Epoch epoch) const override { return source_->at(epoch); }
    const core::EopDataInfo &data_info() const noexcept override { return info_; }

  private:
    std::shared_ptr<const core::IEarthOrientationProvider> source_;
    core::EopDataInfo info_;
};
} // namespace
TEST(Analysis, ProviderSnapshotRetainsExactResourcesThroughAnalysisLifetime) {
    if (!runtime::time_frames_available()) {
        GTEST_SKIP();
    }
    std::weak_ptr<const core::ITimeProvider> time;
    std::weak_ptr<const core::IFrameProvider> frames;
    std::weak_ptr<const core::IEarthOrientationProvider> eop;
    {
        runtime::DataSnapshot captured;
        {
            const auto source = runtime::DataSnapshot::load(ORBELLE_REPORT_EOP, {}, 2);
            time = source.time();
            frames = source.frames();
            eop = source.eop();
            captured =
                runtime::DataSnapshot::from_providers(source.time(), source.frames(), source.eop());
            EXPECT_EQ(captured.data_lock(), source.data_lock());
        }
        EXPECT_EQ(captured.time(), time.lock());
        EXPECT_EQ(captured.frames(), frames.lock());
        EXPECT_EQ(captured.eop(), eop.lock());
        auto document = project();
        const auto ref = station(document);
        const auto analysis = runtime::prepare(document, captured);
        captured = {};
        EXPECT_FALSE(time.expired());
        EXPECT_FALSE(frames.expired());
        EXPECT_FALSE(eop.expired());
        EXPECT_EQ(analysis.data().eop()->data_info().extrapolation_days, 2);
        EXPECT_EQ(analysis.state(ref, core::Epoch{0}, core::FrameId::gcrf).frame,
                  core::FrameId::gcrf);
    }
    EXPECT_TRUE(time.expired());
    EXPECT_TRUE(frames.expired());
    EXPECT_TRUE(eop.expired());
}
TEST(Analysis, ProviderSnapshotRejectsIncompleteAndMismatchedResources) {
    if (!runtime::time_frames_available()) {
        GTEST_SKIP();
    }
    const auto source = runtime::DataSnapshot::load(ORBELLE_REPORT_EOP);
    const auto time_only = runtime::DataSnapshot::from_providers(source.time());
    EXPECT_EQ(time_only.data_lock(), app::time_data_lock(source.time()->data_info()));
    EXPECT_FALSE(time_only.eop());
    EXPECT_FALSE(time_only.frames());
    EXPECT_EQ(runtime::DataSnapshot::from_providers({}).data_lock(), app::DataLock{});
    EXPECT_THROW((void)runtime::DataSnapshot::from_providers(source.time(), source.frames()),
                 std::invalid_argument);
    EXPECT_THROW((void)runtime::DataSnapshot::from_providers(source.time(), {}, source.eop()),
                 std::invalid_argument);
    EXPECT_THROW((void)runtime::DataSnapshot::from_providers({}, source.frames(), source.eop()),
                 std::invalid_argument);
    EXPECT_THROW((void)runtime::DataSnapshot::from_providers(
                     source.time(), source.frames(), std::make_shared<MismatchedEop>(source.eop())),
                 std::invalid_argument);
}
TEST(Analysis, ExactSamplingIncludingEndAtEpochLimits) {
    auto document = project();
    const auto ref = station(document);
    const auto analysis = runtime::prepare(document);
    const auto result = analysis.states(ref, 4ns, core::FrameId::itrf);
    EXPECT_EQ(epochs(result), (std::vector<std::int64_t>{0, 4, 8, 10}));
    EXPECT_DOUBLE_EQ(result.states()[0].position_m[0], 6378137);
    EXPECT_EQ(epochs(analysis.states(ref, 100ns, core::FrameId::itrf)),
              (std::vector<std::int64_t>{0, 10}));
    EXPECT_EQ(epochs(analysis.states(ref, 5ns, core::FrameId::itrf)),
              (std::vector<std::int64_t>{0, 5, 10}));
    EXPECT_EQ(
        epochs(analysis.states(ref, 4ns, core::FrameId::itrf, core::Epoch{3}, core::Epoch{3})),
        (std::vector<std::int64_t>{3}));
    constexpr auto low = std::numeric_limits<std::int64_t>::min();
    constexpr auto high = std::numeric_limits<std::int64_t>::max();
    for (const auto start : {low, std::int64_t{-50}, high - 100}) {
        auto extreme = project(core::Epoch{start}, core::Epoch{start + 100});
        const auto ground = station(extreme);
        const auto fixed = runtime::prepare(extreme);
        EXPECT_EQ(epochs(fixed.states(ground, 40ns, core::FrameId::itrf)),
                  (std::vector<std::int64_t>{start, start + 40, start + 80, start + 100}));
        EXPECT_EQ(epochs(fixed.states(ground, std::chrono::nanoseconds{high}, core::FrameId::itrf)),
                  (std::vector<std::int64_t>{start, start + 100}));
    }
    auto large = project(core::Epoch{0}, core::Epoch{200000});
    const auto ground = station(large);
    EXPECT_THROW((void)runtime::prepare(large).states(ground, 1ns, core::FrameId::itrf),
                 std::invalid_argument);
}
TEST(Analysis, ExplicitOrderEmptySchemaAndInvalidQueries) {
    auto document = project();
    const auto ref = station(document);
    const auto analysis = runtime::prepare(document);
    const std::array times{core::Epoch{9}, core::Epoch{1}, core::Epoch{9}};
    EXPECT_EQ(epochs(analysis.states(ref, times, core::FrameId::itrf)),
              (std::vector<std::int64_t>{9, 1, 9}));
    const auto empty = analysis.states(ref, std::span<const core::Epoch>{}, core::FrameId::itrf);
    EXPECT_TRUE(empty.states().empty());
    EXPECT_EQ(empty.frame(), core::FrameId::itrf);
    EXPECT_EQ(empty.origin(), core::OriginId::earth);
    EXPECT_EQ(empty.provenance().object_id, ref.id());
    EXPECT_EQ(empty.object_name(), "Equator");
    EXPECT_THROW((void)analysis.state(ref, core::Epoch{-1}, core::FrameId::itrf),
                 std::out_of_range);
    EXPECT_THROW((void)analysis.states(ref, 0ns, core::FrameId::itrf), std::invalid_argument);
    EXPECT_THROW((void)analysis.states(ref, -1ns, core::FrameId::itrf), std::invalid_argument);
    EXPECT_THROW(
        (void)analysis.states(ref, 1ns, core::FrameId::itrf, core::Epoch{4}, core::Epoch{2}),
        std::invalid_argument);
    EXPECT_THROW((void)analysis.states(ref, std::span<const core::Epoch>{}, core::FrameId::gcrf),
                 runtime::AnalysisCapabilityError);
    EXPECT_THROW((void)analysis.state(ref, core::Epoch{0}, core::FrameId::spice_j2000),
                 runtime::AnalysisCapabilityError);
}
TEST(Analysis, OwnsOldObjectsAndResultsAfterEditsAndDeletion) {
    auto document = project();
    const auto ref = station(document);
    const auto analysis = runtime::prepare(document);
    const auto original = analysis.state(ref, core::Epoch{5}, core::FrameId::itrf);
    document.objects().update(ref, app::objects::GroundStationUpdate{
                                       "Changed", core::GroundStationLocation{1, 0, 0}, {}});
    EXPECT_NE(runtime::prepare(document).state(ref, core::Epoch{5}, core::FrameId::itrf).position_m,
              original.position_m);
    document.objects().remove(ref);
    EXPECT_EQ(analysis.state(ref, core::Epoch{5}, core::FrameId::itrf).position_m,
              original.position_m);
    EXPECT_EQ(std::get<app::ProjectGroundStation>(analysis.snapshot().read(ref)).name(), "Equator");
    const auto new_ref = station(document);
    EXPECT_THROW((void)analysis.state(new_ref, core::Epoch{0}, core::FrameId::itrf),
                 app::ObjectNotFoundError);
    // Same persisted ID is still a different document scope after reopening/copying.
    app::ProjectDocument other{*analysis.snapshot().project};
    const auto alien = other.objects().get(ref.id());
    EXPECT_THROW((void)analysis.state(alien, core::Epoch{0}, core::FrameId::itrf),
                 app::ProjectError);
}
TEST(Analysis, StalePlansAndNonPhysicalChanges) {
    auto document = project();
    const auto ref = station(document);
    const runtime::AnalysisPlan plan{document};
    const auto result = plan.prepare().states(ref, 5ns, core::FrameId::itrf);
    document.objects().update(ref, app::objects::GroundStationUpdate{"New name", {}, 0.3});
    EXPECT_THROW((void)plan.prepare(true), app::RevisionConflictError);
    const auto current = runtime::prepare(document).states(ref, 5ns, core::FrameId::itrf);
    EXPECT_EQ(result.input_sha256(), current.input_sha256());
    EXPECT_EQ(result.object_name(), "Equator");
    EXPECT_EQ(current.object_name(), "New name");
    EXPECT_EQ(result.states()[0].position_m, current.states()[0].position_m);
}
TEST(Analysis, PinnedDataChangesRequireConsentAndDoNotMutateProject) {
    if (!runtime::time_frames_available())
        GTEST_SKIP();
    const auto data = runtime::DataSnapshot::load(ORBELLE_REPORT_EOP, {}, 2);
    auto raw = app::Project{"Recorded", core::Epoch{0}, core::Epoch{10}};
    raw.earth_orientation = data.eop()->data_info();
    raw.earth_orientation->extrapolation_days = 0;
    app::ProjectDocument document{raw};
    const auto ref = station(document);
    runtime::AnalysisPlan plan{document, data};
    ASSERT_EQ(plan.data_changes().size(), 1U);
    EXPECT_EQ(plan.data_changes()[0].role, "earth-orientation");
    try {
        (void)plan.prepare();
        FAIL() << "Expected an explicit data decision";
    } catch (const runtime::DataChangedError &error) {
        const auto confirmed = error.plan().prepare(true);
        EXPECT_EQ(confirmed.data().eop()->data_info().extrapolation_days, 2);
        EXPECT_EQ(confirmed.state(ref, core::Epoch{0}, core::FrameId::gcrf).frame,
                  core::FrameId::gcrf);
    }
    EXPECT_EQ(document.snapshot().project->earth_orientation->extrapolation_days, 0);
    EXPECT_THROW((void)plan.prepare(), runtime::DataChangedError); // No sticky consent.
    // Unused saved EOP is not required for a native ITRF station query.
    EXPECT_EQ(runtime::prepare(document).state(ref, core::Epoch{0}, core::FrameId::itrf).frame,
              core::FrameId::itrf);
}
TEST(Analysis, CoverageAndProviderConsistencyAreValidatedBeforeConsent) {
    if (!runtime::time_frames_available())
        GTEST_SKIP();
    const auto strict = runtime::DataSnapshot::load(ORBELLE_REPORT_EOP);
    const auto end = strict.eop()->data_info().last_epoch.tai_nanoseconds();
    auto outside = project(core::Epoch{end + 1}, core::Epoch{end + 1'000'000'000});
    const auto ref = station(outside);
    EXPECT_THROW((void)runtime::AnalysisPlan(outside, strict), core::EopCoverageError);
    const auto extended = runtime::DataSnapshot::load(ORBELLE_REPORT_EOP, {}, 1);
    const auto analysis = runtime::prepare(outside, extended);
    EXPECT_EQ(analysis.state(ref, outside.snapshot().project->last, core::FrameId::gcrf).frame,
              core::FrameId::gcrf);
    const auto future =
        project(core::Epoch{8'000'000'000'000'000'000}, core::Epoch{8'000'000'000'000'000'010});
    EXPECT_THROW((void)runtime::AnalysisPlan(future, runtime::DataSnapshot::builtin_time()),
                 core::TimeDataRangeError);
    EXPECT_THROW((void)runtime::DataSnapshot::load(ORBELLE_REPORT_EOP, {}, 31),
                 std::invalid_argument);
    EXPECT_THROW((void)runtime::DataSnapshot::load(), std::invalid_argument);
}
TEST(Analysis, PropagationAndStateTransformShareExistingNumericalImplementation) {
    if (!runtime::two_body_available() || !runtime::time_frames_available())
        GTEST_SKIP();
    auto document = project(core::Epoch{0}, core::Epoch{60'000'000'000});
    const auto ref = document.objects().create(app::objects::Satellite{"Observer", {}, {}});
    const auto data = runtime::DataSnapshot::load(ORBELLE_REPORT_EOP);
    const auto analysis = runtime::prepare(document, data);
    const auto epoch = core::Epoch{42'000'000'000};
    const auto expected =
        runtime::make_orbit_session(document.snapshot().project->satellites[0].orbit)
            .sample(std::array{epoch})
            .states()[0];
    EXPECT_EQ(analysis.state(ref, epoch, core::FrameId::gcrf).position_m, expected.position_m);
    const auto transformed =
        core::transform_state(expected, core::FrameId::itrf, *data.frames(), data.eop()->at(epoch));
    const auto actual = analysis.state(ref, epoch, core::FrameId::itrf);
    EXPECT_EQ(actual.position_m, transformed.position_m);
    EXPECT_EQ(actual.velocity_mps, transformed.velocity_mps);
    EXPECT_EQ(analysis.states(ref, 30s, core::FrameId::gcrf).data_sha256(),
              app::DataLock{}.sha256());
    EXPECT_EQ(analysis.states(ref, 30s, core::FrameId::itrf).data_sha256(),
              data.data_lock().sha256());
    document.objects().rename(ref, "Renamed");
    EXPECT_EQ(analysis.states(ref, 30s, core::FrameId::gcrf).input_sha256(),
              runtime::prepare(document).states(ref, 30s, core::FrameId::gcrf).input_sha256());
}
