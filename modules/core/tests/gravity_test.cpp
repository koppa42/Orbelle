#include <cmath>
#include <fstream>
#include <gtest/gtest.h>
#include <orbitforge/core/propagation/gravity.hpp>
#include <sstream>
using namespace orbitforge::core;
TEST(Gravity, HighDegreeNormalizedFieldMatchesIndependentOrekitIncludingNearPoles) {
    std::vector<GravityCoefficient> coefficients;
    for (int n = 2; n <= 360; ++n)
        for (int m = 0; m <= n; ++m) {
            const double amplitude = 1e-8 / ((n + 1.0) * (n + 1));
            coefficients.push_back(
                {n, m,
                 n == 2 && m == 0 ? -4.84165371736e-4 : amplitude * std::sin((n + 1.0) * (m + 2)),
                 m == 0 ? 0 : amplitude * std::cos((n + 1.0) * (m + 2))});
        }
    GravityModel model({"synthetic", {}, {}, "tide_free", 3.986004415e14, 6378136.3, 360, 360},
                       coefficients);
    std::ifstream file(ORBITFORGE_GRAVITY_REFERENCE);
    ASSERT_TRUE(file.is_open());
    int rows = 0;
    for (std::string line; std::getline(file, line);) {
        if (line.empty() || line[0] == '#')
            continue;
        std::istringstream input(line);
        int degree = 0, order = 0;
        std::array<double, 3> position{}, expected{};
        input >> degree >> order;
        for (auto &v : position)
            input >> v;
        for (auto &v : expected)
            input >> v;
        ASSERT_TRUE(input);
        const auto actual = model.acceleration(position, degree, order);
        for (int axis = 0; axis < 3; ++axis)
            EXPECT_NEAR(actual[axis], expected[axis], 5e-12) << line;
        ++rows;
    }
    EXPECT_EQ(rows, 40);
    const auto exact = model.acceleration({0, 0, 7e6}, 360, 360);
    const auto near = model.acceleration({1e-7, 0, 7e6}, 360, 360);
    for (int axis = 0; axis < 3; ++axis)
        EXPECT_NEAR(exact[axis], near[axis], 5e-12);
    EXPECT_THROW((void)model.acceleration({7e6, 0, 0}, 361, 360), std::invalid_argument);
}
