#include <cmath>
#include <fstream>
#include <gtest/gtest.h>
#include <limits>
#include <numbers>
#include <orbitforge/core/propagation/orbit_design.hpp>
#include <sstream>

using namespace orbitforge::core;
namespace {
constexpr double pi = std::numbers::pi, rad = pi / 180;
TEST(OrbitDesign, MatchesIndependentHighPrecisionRoots) {
    std::ifstream file(ORBITFORGE_ORBIT_DESIGN_REFERENCE);
    ASSERT_TRUE(file);
    std::string line;
    int count = 0;
    while (std::getline(file, line)) {
        if (line.empty() || line[0] == '#')
            continue;
        std::istringstream row(line);
        int type;
        double a, e, i, node, period;
        ASSERT_TRUE(static_cast<bool>(row >> type >> a >> e >> i >> node >> period));
        EarthOrbitDesignRequest q;
        q.type = static_cast<EarthOrbitType>(type);
        const auto result = design_earth_orbit(q);
        EXPECT_NEAR(result.elements.semi_major_axis_m, a, 1e-6);
        EXPECT_NEAR(result.elements.eccentricity, e, 1e-13);
        EXPECT_NEAR(result.elements.inclination_rad, i, 1e-12);
        EXPECT_NEAR(result.raan_rate_rad_s, node, 1e-18);
        EXPECT_NEAR(result.nodal_period_seconds, period, 1e-8);
        ++count;
    }
    EXPECT_EQ(count, 5);
}
TEST(OrbitDesign, CircularCustomAndSynchronousHaveAnalyticSize) {
    EarthOrbitDesignRequest q;
    q.epoch = Epoch{12345};
    auto orbit = design_earth_orbit(q);
    EXPECT_DOUBLE_EQ(orbit.elements.semi_major_axis_m, 6878136.6);
    EXPECT_DOUBLE_EQ(orbit.elements.eccentricity, 0);
    EXPECT_EQ(orbit.elements.epoch, q.epoch);
    EXPECT_FALSE(orbit.secular_j2);
    q.type = EarthOrbitType::orbit_designer;
    q.semi_major_axis_m = 8000000;
    q.eccentricity = .1;
    q.raan_rad = -20 * rad;
    orbit = design_earth_orbit(q);
    EXPECT_DOUBLE_EQ(orbit.elements.semi_major_axis_m, q.semi_major_axis_m);
    EXPECT_DOUBLE_EQ(orbit.elements.eccentricity, .1);
    EXPECT_NEAR(orbit.elements.raan_rad, 340 * rad, 1e-14);
    q.type = EarthOrbitType::geosynchronous;
    orbit = design_earth_orbit(q);
    EXPECT_NEAR(orbit.nodal_period_seconds, 86164.10063718943, 1e-8);
    EXPECT_NEAR(orbit.elements.semi_major_axis_m, 42164172.93115725, 1e-6);
    q.inclination_rad = pi / 2;
    EXPECT_THROW((void)design_earth_orbit(q), OrbitDesignError);
}
TEST(OrbitDesign, CriticalInclinationsFreezeApsidesInBothDirections) {
    EarthOrbitDesignRequest q;
    q.type = EarthOrbitType::critically_inclined;
    for (bool reverse : {false, true}) {
        q.retrograde = reverse;
        const auto orbit = design_earth_orbit(q);
        EXPECT_NEAR(orbit.elements.inclination_rad / rad,
                    reverse ? 116.565051177078 : 63.434948822922, 1e-10);
        EXPECT_NEAR(orbit.periapsis_rate_rad_s, 0, 1e-20);
        EXPECT_NEAR(orbit.elements.semi_major_axis_m * (1 - orbit.elements.eccentricity), 6878136.6,
                    1e-7);
        EXPECT_NEAR(orbit.elements.semi_major_axis_m * (1 + orbit.elements.eccentricity),
                    46378136.6, 1e-7);
    }
}
TEST(OrbitDesign, SunSynchronousSolvesEitherConditionAndCriticalCombination) {
    EarthOrbitDesignRequest q;
    q.type = EarthOrbitType::sun_synchronous;
    q.altitude_m = 7000000 - 6378136.6;
    const auto sun = design_earth_orbit(q);
    EXPECT_NEAR(sun.raan_rate_rad_s * 86400 / rad, 360 / 365.2422, 1e-12);
    EXPECT_GT(sun.elements.inclination_rad, 97 * rad);
    EXPECT_LT(sun.elements.inclination_rad, 99 * rad);
    q.sun_sync_by_inclination = true;
    q.inclination_rad = sun.elements.inclination_rad;
    EXPECT_NEAR(design_earth_orbit(q).elements.semi_major_axis_m, 7000000, 1e-7);
    q.type = EarthOrbitType::critically_inclined_sun_sync;
    const auto combined = design_earth_orbit(q);
    EXPECT_NEAR(combined.periapsis_rate_rad_s, 0, 1e-20);
    EXPECT_NEAR(combined.raan_rate_rad_s * 86400 / rad, 360 / 365.2422, 1e-12);
    EXPECT_NEAR(combined.elements.semi_major_axis_m * (1 - combined.elements.eccentricity),
                6878136.6, 1e-7);
}
TEST(OrbitDesign, MolniyaUsesHalfSiderealDayAndPerigeeConstraint) {
    EarthOrbitDesignRequest q;
    q.type = EarthOrbitType::molniya;
    q.argument_of_periapsis_rad = 270 * rad;
    const auto orbit = design_earth_orbit(q);
    EXPECT_NEAR(orbit.nodal_period_seconds, 43082.050318594716, 1e-8);
    EXPECT_NEAR(orbit.periapsis_rate_rad_s, 0, 1e-20);
    EXPECT_NEAR(orbit.elements.semi_major_axis_m * (1 - orbit.elements.eccentricity), 6878136.6,
                1e-7);
    EXPECT_DOUBLE_EQ(orbit.elements.argument_of_periapsis_rad, q.argument_of_periapsis_rad);
    EXPECT_GT(orbit.elements.eccentricity, .7);
}
TEST(OrbitDesign, RepeatConstraintClosesForProgradePolarRetrogradeAndSunSynchronous) {
    EarthOrbitDesignRequest q;
    for (const auto type :
         {EarthOrbitType::repeating_ground_trace, EarthOrbitType::repeating_sun_sync}) {
        q.type = type;
        for (const double inclination : {0., 53 * rad, pi / 2, 100 * rad, pi}) {
            q.inclination_rad = inclination;
            for (const auto pair : {std::pair{15, 1}, std::pair{233, 16}, std::pair{29, 2}}) {
                q.repeat_revolutions = pair.first;
                q.repeat_days = pair.second;
                const auto orbit = design_earth_orbit(q);
                EXPECT_NEAR((7.292115e-5 - orbit.raan_rate_rad_s) * orbit.repeat_cycle_seconds,
                            2 * pi * pair.second, 1e-10);
                EXPECT_NEAR(orbit.repeat_cycle_seconds / orbit.nodal_period_seconds, pair.first,
                            1e-10);
                if (type == EarthOrbitType::repeating_sun_sync)
                    EXPECT_NEAR(orbit.raan_rate_rad_s * 86400 / rad, 360 / 365.2422, 1e-12);
            }
        }
    }
}
TEST(OrbitDesign, InvalidAndUnreachableConstraintsAreRejected) {
    EarthOrbitDesignRequest q;
    for (const double bad : {0., -1., std::numeric_limits<double>::quiet_NaN(),
                             std::numeric_limits<double>::infinity()}) {
        q.altitude_m = bad;
        EXPECT_THROW((void)design_earth_orbit(q), OrbitDesignError);
    }
    q = {};
    q.type = EarthOrbitType::orbit_designer;
    q.eccentricity = .5;
    EXPECT_THROW((void)design_earth_orbit(q), OrbitDesignError);
    q.type = EarthOrbitType::sun_synchronous;
    q.altitude_m = 50000000;
    EXPECT_THROW((void)design_earth_orbit(q), OrbitDesignError);
    q.sun_sync_by_inclination = true;
    q.inclination_rad = pi / 2;
    EXPECT_THROW((void)design_earth_orbit(q), OrbitDesignError);
    q.type = EarthOrbitType::critically_inclined_sun_sync;
    q.perigee_altitude_m = 50000000;
    EXPECT_THROW((void)design_earth_orbit(q), OrbitDesignError);
    q.type = EarthOrbitType::molniya;
    EXPECT_THROW((void)design_earth_orbit(q), OrbitDesignError);
    q = {};
    q.type = EarthOrbitType::critically_inclined;
    q.apogee_altitude_m = 100000;
    EXPECT_THROW((void)design_earth_orbit(q), OrbitDesignError);
    q.type = EarthOrbitType::repeating_ground_trace;
    q.repeat_days = 0;
    EXPECT_THROW((void)design_earth_orbit(q), OrbitDesignError);
    q.repeat_days = 1;
    q.repeat_revolutions = 1000;
    EXPECT_THROW((void)design_earth_orbit(q), OrbitDesignError);
    q.type = static_cast<EarthOrbitType>(100);
    EXPECT_THROW((void)design_earth_orbit(q), OrbitDesignError);
}
} // namespace
