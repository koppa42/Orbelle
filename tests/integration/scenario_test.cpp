#include <gtest/gtest.h>
#include <orbitforge/application/scenario.hpp>
#include <orbitforge/core/propagation/circular_two_body.hpp>

using namespace orbitforge;
namespace {
application::Session object(const char *id, std::uint64_t revision = 1) {
    return {std::make_shared<core::CircularTwoBody>(
                core::Length{7e6}, core::GravitationalParameter{3.986004418e14}, core::Epoch{0}),
            results::Provenance{id, revision, "scenario-test-circular"}};
}
} // namespace
TEST(Scenario, StartsEmptyAndOwnsOneAnalysisInterval) {
    const application::Scenario scenario("Mission", core::Epoch{-10}, core::Epoch{10});
    EXPECT_TRUE(scenario.objects().empty());
    EXPECT_TRUE(scenario.sample(std::array{core::Epoch{0}}).empty());
    EXPECT_THROW((void)scenario.sample(std::array{core::Epoch{11}}), std::out_of_range);
    EXPECT_THROW((void)application::Scenario(" ", core::Epoch{0}, core::Epoch{10}),
                 std::invalid_argument);
    EXPECT_THROW((void)application::Scenario("Mission", core::Epoch{10}, core::Epoch{0}),
                 std::invalid_argument);
}
TEST(Scenario, IndependentObjectsAndRevisionsPreserveReaders) {
    const application::Scenario empty("Mission", core::Epoch{0}, core::Epoch{10'000'000'000});
    const auto first = empty.with_object(object("A"));
    const auto second = first.with_object(object("B"));
    const auto edited = second.with_object(object("A", 2));
    EXPECT_EQ(empty.objects().size(), 0U);
    EXPECT_EQ(first.objects().size(), 1U);
    EXPECT_EQ(second.objects()[0].provenance().input_revision, 1U);
    EXPECT_EQ(edited.objects().size(), 2U);
    EXPECT_EQ(edited.objects()[0].provenance().input_revision, 2U);
    EXPECT_EQ(edited.objects()[1].provenance().input_revision, 1U);
    EXPECT_THROW((void)second.with_object(object("A")), std::invalid_argument);
    EXPECT_THROW((void)second.with_object(object("A", 3)), std::invalid_argument);
    const auto states = edited.sample(std::array{core::Epoch{0}, core::Epoch{10'000'000'000}});
    ASSERT_EQ(states.size(), 2U);
    EXPECT_EQ(states[0].provenance().object_id, "A");
    EXPECT_EQ(states[1].provenance().object_id, "B");
    EXPECT_EQ(states[0].states().size(), 2U);
}
TEST(Scenario, RemovalPreservesSurvivingRevisionsAndExistingReaders) {
    const auto original = application::Scenario("Mission", core::Epoch{0}, core::Epoch{100})
                              .with_object(object("A"))
                              .with_object(object("B"))
                              .with_object(object("B", 2));
    const auto removed = original.without_object("A");
    EXPECT_EQ(original.objects().size(), 2U);
    ASSERT_EQ(removed.objects().size(), 1U);
    EXPECT_EQ(removed.objects()[0].provenance().object_id, "B");
    EXPECT_EQ(removed.objects()[0].provenance().input_revision, 2U);
    EXPECT_EQ(removed.revision(), original.revision() + 1);
    EXPECT_EQ(removed.sample(std::array{core::Epoch{0}})[0].states()[0].position_m,
              original.sample(std::array{core::Epoch{0}})[1].states()[0].position_m);
    EXPECT_TRUE(removed.without_object("B").objects().empty());
    EXPECT_THROW((void)removed.without_object("A"), std::invalid_argument);
}
TEST(Scenario, DataBindingCreatesANewScenarioRevisionAndKeepsReadersAndSessions) {
    const auto original =
        application::Scenario("Mission", core::Epoch{0}, core::Epoch{100}).with_object(object("A"));
    const application::DataLock lock(
        {{"utc-time", "test", std::string(64, 'a'), "test-v1", std::string(64, 'b')}});
    const auto changed = original.with_data_lock(lock);
    EXPECT_TRUE(original.data_lock().references().empty());
    EXPECT_EQ(changed.data_lock(), lock);
    EXPECT_EQ(changed.revision(), original.revision() + 1);
    EXPECT_EQ(changed.objects()[0].provenance().input_revision, 1U);
    EXPECT_EQ(changed.with_data_lock(lock).revision(), changed.revision());
    EXPECT_EQ(changed.with_object(object("B")).data_lock(), lock);
    EXPECT_EQ(changed.without_object("A").data_lock(), lock);
}
