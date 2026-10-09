#include "workload.hpp"
#include <cmath>
#include <gtest/gtest.h>
#include <orbitforge/runtime/time_frames.hpp>
#include <set>

namespace {
using namespace orbitforge;
double length(const std::array<double, 3> &p) { return std::hypot(p[0], p[1], p[2]); }
TEST(Constellation, TenThousandUniqueObjectsKeepSpecifiedOrbitalRadii) {
    const benchmarks::Constellation constellation;
    const auto first = constellation.at(core::Epoch{0});
    const auto later = constellation.at(core::Epoch{123'000'000'000});
    ASSERT_EQ(first.markers.size(), 10000u);
    EXPECT_EQ(first.frame, core::FrameId::gcrf);
    std::set<std::string> ids;
    for (std::size_t i = 0; i < first.markers.size(); ++i) {
        EXPECT_TRUE(ids.insert(first.markers[i].object_id).second);
        const double expected = 6378137.0 + 550000.0 + 40000.0 * static_cast<double>(i / 1000);
        EXPECT_NEAR(length(first.markers[i].position_m), expected, 1e-7);
        EXPECT_NEAR(length(later.markers[i].position_m), expected, 1e-7);
        EXPECT_NE(first.markers[i].position_m, later.markers[i].position_m);
    }
    EXPECT_DOUBLE_EQ(first.markers.front().position_m[0], 6928137.0);
    EXPECT_DOUBLE_EQ(first.markers.front().position_m[1], 0.0);
    EXPECT_DOUBLE_EQ(first.markers.front().position_m[2], 0.0);
}
TEST(DisplayCache, GroundFixedSamplesHaveBoundedInterpolationAndStrictCoverage) {
    const auto frames = runtime::make_frame_provider();
    const auto eop = runtime::load_eop_snapshot(ORBITFORGE_BENCHMARK_EOP);
    const benchmarks::DisplayCache cache(*frames, *eop);
    EXPECT_EQ(cache.position_bytes(), 601u * 10000u * 3u * sizeof(double));
    const core::Epoch halfway{321'500'000'000};
    const auto cached = cache.sample(halfway);
    const auto exact =
        visualization::rotate_snapshot(benchmarks::Constellation{}.at(halfway),
                                       frames->gcrf_to_itrf(halfway, eop->at(halfway).sample));
    ASSERT_EQ(cached.markers.size(), exact.markers.size());
    EXPECT_EQ(cached.epoch, halfway);
    EXPECT_EQ(cached.frame, core::FrameId::itrf);
    // Independent circular chord bound is under 1.3 m for these radii/rates.
    // The more generous 2 m limit also covers Earth rotation and all inclinations.
    for (std::size_t i = 0; i < cached.markers.size(); ++i) {
        std::array<double, 3> delta{};
        for (std::size_t k = 0; k < 3; ++k)
            delta[k] = cached.markers[i].position_m[k] - exact.markers[i].position_m[k];
        EXPECT_LT(length(delta), 2.0);
    }
    EXPECT_EQ(cache.sample(core::Epoch{0}).markers.front().object_id, "SAT-00000");
    EXPECT_EQ(cache.sample(core::Epoch{600'000'000'000}).markers.size(), 10000u);
    EXPECT_THROW((void)cache.sample(core::Epoch{-1}), std::out_of_range);
    EXPECT_THROW((void)cache.sample(core::Epoch{600'000'000'001}), std::out_of_range);
    const auto track = cache.track(1234);
    ASSERT_EQ(track.size(), 121u);
    EXPECT_EQ(track.front().markers.front().object_id, "SAT-01234");
    EXPECT_EQ(track.back().epoch, core::Epoch{600'000'000'000});
    EXPECT_THROW((void)cache.track(10000), std::out_of_range);
}
} // namespace
