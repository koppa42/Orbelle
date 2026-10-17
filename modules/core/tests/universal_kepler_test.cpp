#include "../src/propagation/universal_kepler.hpp"
#include <fstream>
#include <gtest/gtest.h>
#include <sstream>

using namespace orbitforge::core;
TEST(UniversalKepler, MatchesIndependentOrekitAcrossAllConicsAndDirections) {
    std::ifstream input(ORBITFORGE_UNIVERSAL_KEPLER_REFERENCE);
    ASSERT_TRUE(input);
    std::string line;
    std::size_t count = 0;
    double max_position = 0, max_velocity = 0, max_roundtrip = 0;
    while (std::getline(input, line)) {
        if (line.empty() || line.front() == '#')
            continue;
        int kind;
        double mu, time;
        detail::KeplerState initial{}, expected{};
        std::istringstream row(line);
        row >> kind >> mu >> time;
        for (auto &v : initial)
            row >> v;
        for (auto &v : expected)
            row >> v;
        ASSERT_TRUE(row);
        SCOPED_TRACE(line);
        const auto actual = detail::universal_kepler(initial, time, mu);
        const auto restored = detail::universal_kepler(actual, -time, mu);
        for (std::size_t i = 0; i < 6; ++i) {
            const double error = std::abs(actual[i] - expected[i]);
            if (i < 3) {
                max_position = std::max(max_position, error);
                max_roundtrip = std::max(max_roundtrip, std::abs(restored[i] - initial[i]));
                EXPECT_NEAR(actual[i], expected[i], 5e-4);
                EXPECT_NEAR(restored[i], initial[i], 5e-4);
            } else {
                max_velocity = std::max(max_velocity, error);
                EXPECT_NEAR(actual[i], expected[i], 5e-7);
                EXPECT_NEAR(restored[i], initial[i], 5e-7);
            }
        }
        ++count;
    }
    EXPECT_EQ(count, 80U);
    RecordProperty("position_error_m", max_position);
    RecordProperty("velocity_error_mps", max_velocity);
    RecordProperty("roundtrip_position_m", max_roundtrip);
}
TEST(UniversalKepler, RejectsInvalidGeometryAndSupportsCancellation) {
    const detail::KeplerState initial{7e6, 0, 0, 0, 7500, 0};
    constexpr double mu = 3.986004415e14;
    EXPECT_EQ(detail::universal_kepler(initial, 0, mu), initial);
    EXPECT_THROW(detail::universal_kepler(initial, 10, 0), std::invalid_argument);
    EXPECT_THROW(detail::universal_kepler({}, 10, mu), std::invalid_argument);
    EXPECT_THROW(detail::universal_kepler({7e6, 0, 0, 1000, 0, 0}, 10, mu), std::invalid_argument);
    EXPECT_THROW(detail::universal_kepler(initial, std::numeric_limits<double>::infinity(), mu),
                 std::invalid_argument);
    int checks = 0;
    EXPECT_THROW(detail::universal_kepler(initial, 6000, mu,
                                          [&] {
                                              if (++checks == 3)
                                                  throw std::runtime_error("cancelled");
                                          }),
                 std::runtime_error);
    EXPECT_EQ(checks, 3);
    EXPECT_NO_THROW(detail::universal_kepler(initial, 6000, mu));
}
