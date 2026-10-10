#include <fstream>
#include <gtest/gtest.h>
#include <limits>
#include <orbitforge/runtime/project.hpp>

using namespace orbitforge;
namespace {
application::Project example() {
    application::Project project{"任务 A", core::Epoch{123456789}, core::Epoch{7200123456789}};
    project.current = core::Epoch{456123456789};
    for (const auto kind :
         {application::OrbitPropagator::two_body, application::OrbitPropagator::j2_secular}) {
        application::ProjectSatellite satellite;
        satellite.orbit.object_id =
            kind == application::OrbitPropagator::two_body ? "TwoBody" : "长期项";
        satellite.orbit.propagator = kind;
        satellite.orbit.initial.epoch = project.current;
        satellite.orbit.first = project.first;
        satellite.orbit.last = project.last;
        satellite.orbit.input_frame = core::FrameId::itrf;
        satellite.display.extent = 2;
        satellite.display.past_minutes = 12.345;
        satellite.display.labels_2d = false;
        satellite.model_kind = 2;
        satellite.model_path = "模型/satellite.glb";
        project.satellites.push_back(satellite);
    }
    project.earth_orientation = core::EopDataInfo{
        "eop-test",   std::string(64, 'a'),
        "source",     "ITRF2020",
        "hermite-v1", core::Epoch{0},
        project.last, {"time-test", std::string(64, 'b'), "time-source", 40000, 70000}};
    return project;
}
} // namespace
TEST(Project, BinaryRoundTripPreservesExactInputsDisplayDataAndNanoseconds) {
    const auto original = example();
    const auto bytes = application::encode_project(original);
    const auto restored = application::decode_project(bytes);
    EXPECT_EQ(application::encode_project(restored), bytes);
    EXPECT_EQ(restored.name, original.name);
    EXPECT_EQ(restored.current, original.current);
    ASSERT_EQ(restored.satellites.size(), 2U);
    EXPECT_EQ(restored.satellites[1].orbit.initial.velocity_mps,
              original.satellites[1].orbit.initial.velocity_mps);
    EXPECT_EQ(restored.satellites[1].display, original.satellites[1].display);
    EXPECT_EQ(restored.satellites[1].model_path, original.satellites[1].model_path);
    EXPECT_TRUE(application::matching_project_data(*restored.earth_orientation,
                                                   *original.earth_orientation));
    auto data = *restored.earth_orientation;
    data.interpolation = "different";
    EXPECT_FALSE(application::matching_project_data(data, *original.earth_orientation));
    data = *restored.earth_orientation;
    data.time_data.sha256[0] = 'c';
    EXPECT_FALSE(application::matching_project_data(data, *original.earth_orientation));
}
TEST(Project, RejectsTruncationCorruptionFutureVersionAndTrailingBytes) {
    const auto bytes = application::encode_project(example());
    for (std::size_t size = 0; size < bytes.size(); ++size)
        EXPECT_THROW((void)application::decode_project(std::span(bytes).first(size)),
                     application::ProjectError);
    for (std::size_t offset = 0; offset < bytes.size(); ++offset) {
        auto bad = bytes;
        bad[offset] ^= 0x40;
        EXPECT_THROW((void)application::decode_project(bad), application::ProjectError);
    }
    auto bad = bytes;
    bad.push_back(0);
    EXPECT_THROW((void)application::decode_project(bad), application::ProjectError);
}
TEST(Project, RejectsAmbiguousInputsAndInvalidReferencesBeforeWriting) {
    auto project = example();
    project.satellites[1].orbit.object_id = project.satellites[0].orbit.object_id;
    EXPECT_THROW(application::validate_project(project), application::ProjectError);
    project = example();
    project.satellites[0].orbit.initial.position_m[1] = std::numeric_limits<double>::quiet_NaN();
    EXPECT_THROW(application::validate_project(project), application::ProjectError);
    project = example();
    project.current = core::Epoch{-1};
    EXPECT_THROW(application::validate_project(project), application::ProjectError);
    project = example();
    project.satellites[0].orbit.first = core::Epoch{-1};
    EXPECT_THROW(application::validate_project(project), application::ProjectError);
    project = example();
    project.name = std::string("\xc0\xaf");
    EXPECT_THROW(application::validate_project(project), application::ProjectError);
    project = example();
    project.earth_orientation->last_epoch = project.first;
    EXPECT_THROW(application::validate_project(project), application::ProjectError);
}
TEST(Project, FileReplacementIsExplicitAndFailedSavePreservesExistingProject) {
    const auto dir = std::filesystem::path(ORBITFORGE_INTEGRATION_WORKSPACE) / "project-file-test";
    std::filesystem::create_directories(dir);
    const auto path = dir / std::filesystem::path(u8"任务.orbitforge");
    const auto original = example();
    application::save_project(path, original, true);
    auto edited = original;
    edited.name = "changed";
    EXPECT_THROW(application::save_project(path, edited), application::ProjectError);
    EXPECT_EQ(application::load_project(path).name, original.name);
    edited.satellites[0].orbit.step_seconds = 0;
    EXPECT_THROW(application::save_project(path, edited, true), application::ProjectError);
    EXPECT_EQ(application::load_project(path).name, original.name);
    edited.satellites[0].orbit.step_seconds = 30;
    application::save_project(path, edited, true);
    EXPECT_EQ(application::load_project(path).name, edited.name);
    EXPECT_THROW(application::save_project(dir, edited, true), std::exception);
    EXPECT_EQ(std::distance(std::filesystem::directory_iterator(dir),
                            std::filesystem::directory_iterator{}),
              1);
    std::filesystem::remove(path);
    std::filesystem::remove(dir);
}
TEST(Project, ReconstructedTwoBodyAndJ2StatesAreIdenticalInEitherTimeDirection) {
#if ORBITFORGE_EXPECT_SPICE
    const auto original = example();
    const auto restored = application::decode_project(application::encode_project(original));
    const auto before = runtime::make_project_scenario(original);
    const auto after = runtime::make_project_scenario(restored);
    const std::array epochs{original.first, original.current, original.last};
    const auto a = before.sample(epochs), b = after.sample(epochs);
    ASSERT_EQ(a.size(), b.size());
    for (std::size_t i = 0; i < a.size(); ++i)
        for (std::size_t j = 0; j < epochs.size(); ++j) {
            EXPECT_EQ(a[i].states()[j].position_m, b[i].states()[j].position_m);
            EXPECT_EQ(a[i].states()[j].velocity_mps, b[i].states()[j].velocity_mps);
            EXPECT_EQ(a[i].provenance().algorithm, b[i].provenance().algorithm);
        }
#else
    EXPECT_THROW((void)runtime::make_project_scenario(example()), std::runtime_error);
#endif
}
TEST(Project, EmptyProjectAndExtremeEpochsRoundTripWithoutRounding) {
    const auto start = std::numeric_limits<std::int64_t>::max() - 100;
    const application::Project empty{"Empty", core::Epoch{start}, core::Epoch{start + 1}};
    const auto loaded = application::decode_project(application::encode_project(empty));
    EXPECT_EQ(loaded.first, empty.first);
    EXPECT_EQ(loaded.last, empty.last);
    EXPECT_TRUE(loaded.satellites.empty());
    EXPECT_TRUE(runtime::make_project_scenario(loaded).objects().empty());
}
