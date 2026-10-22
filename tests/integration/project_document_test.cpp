#include <gtest/gtest.h>
#include <orbelle/application/project_document.hpp>
using namespace orbelle::application;
using orbelle::core::Epoch;
namespace {
ProjectDocument document() {
    return ProjectDocument{Project{"Objects", Epoch{0}, Epoch{60'000'000'000}}};
}
} // namespace
TEST(ProjectDocument, AtomicChangesAndStableSnapshots) {
    auto project = document();
    std::vector<ChangeSet> changes;
    const auto token = project.subscribe([&](const auto &change) { changes.push_back(change); });
    auto edit = project.edit();
    const auto satellite = edit.objects().create(objects::Satellite{"Sat", {}, {}});
    const auto station = edit.objects().create(objects::GroundStation{"Station", {}, 0});
    const auto sensor = edit.objects().create(objects::Sensor{"Camera", satellite, 0.2, {station}});
    EXPECT_TRUE(project.objects().find().empty());
    edit.commit();
    ASSERT_EQ(changes.size(), 1);
    EXPECT_EQ(changes[0].added.size(), 3);
    const auto old = project.snapshot();
    EXPECT_THROW(project.objects().update(sensor, objects::SensorUpdate{"Broken", -1, {}}),
                 ProjectError);
    EXPECT_EQ(project.snapshot().revision, 1);
    EXPECT_EQ(std::get<ProjectSensor>(project.objects().read(sensor)).name(), "Camera");
    project.objects().rename(sensor, "Camera");
    EXPECT_EQ(changes.size(), 1);
    project.objects().rename(sensor, "Changed");
    EXPECT_EQ(old.project->sensors[0].name(), "Camera");
    EXPECT_EQ(changes.back().updated[0].fields, std::vector<std::string>{"name"});
    project.unsubscribe(token);
}
TEST(ProjectDocument, RollbackDeletionAndConflicts) {
    auto project = document();
    auto satellite = project.objects().create(objects::Satellite{"Sat", {}, {}});
    auto station = project.objects().create(objects::GroundStation{"Station", {}, 0});
    auto sensor = project.objects().create(objects::Sensor{"Camera", satellite, 0.2, {station}});
    const auto plan = project.objects().plan_remove(station);
    EXPECT_EQ(plan.blocking_sensors, std::vector<std::string>{sensor.id()});
    EXPECT_THROW(project.objects().remove(station), ProjectError);
    auto edit = project.edit();
    edit.objects().remove(station); // References need only be resolved at commit.
    edit.objects().update(sensor, objects::SensorUpdate{{}, {}, objects::Pointing{}});
    edit.commit();
    EXPECT_THROW((void)project.objects().read(station), ObjectNotFoundError);
    auto stale = project.edit();
    stale.objects().rename(sensor, "Stale");
    project.objects().rename(sensor, "Current");
    EXPECT_THROW(stale.commit(), RevisionConflictError);
    EXPECT_THROW((void)stale.objects(), EditClosedError);
    auto other = document();
    EXPECT_THROW((void)other.objects().read(sensor), ProjectError);
    auto discarded = project.edit();
    auto temporary = discarded.objects().create(objects::GroundStation{"Temporary", {}, 0});
    discarded.discard();
    EXPECT_THROW((void)project.objects().read(temporary), ObjectNotFoundError);
    const auto copy = project.objects().duplicate(satellite, "Copy");
    ASSERT_EQ(project.objects().children(copy).size(), 1);
    EXPECT_NE(project.objects().children(copy)[0].id(), sensor.id());
    project.objects().remove(satellite);
    EXPECT_THROW((void)project.objects().read(sensor), ObjectNotFoundError);
    EXPECT_EQ(project.objects().children(copy).size(), 1);
}
TEST(ProjectDocument, NoopDirtyAndObservers) {
    auto project = document();
    auto station = project.objects().create(objects::GroundStation{"Station", {}, 0});
    project.mark_saved(project.snapshot().revision);
    EXPECT_FALSE(project.dirty());
    auto edit = project.edit();
    edit.objects().rename(station, "Draft");
    edit.objects().rename(station, "Station");
    edit.commit();
    EXPECT_FALSE(project.dirty());
    std::vector<std::uint64_t> order;
    const auto first = project.subscribe([&](const ChangeSet &changes) {
        if (changes.revision == 2)
            project.objects().rename(station, "Reentrant");
        throw std::runtime_error("observer failed");
    });
    const auto second =
        project.subscribe([&](const ChangeSet &changes) { order.push_back(changes.revision); });
    project.objects().rename(station, "Published");
    EXPECT_EQ(order, (std::vector<std::uint64_t>{2, 3}));
    EXPECT_EQ(project.observer_errors().size(), 2);
    EXPECT_TRUE(project.dirty());
    project.unsubscribe(first);
    project.unsubscribe(second);
}
