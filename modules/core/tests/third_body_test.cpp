#include <cmath>
#include <fstream>
#include <gtest/gtest.h>
#include <numbers>
#include <orbitforge/core/propagation/hpop.hpp>
#include <sstream>

using namespace orbitforge::core;
namespace {
class MovingBodies final : public IThirdBodyEphemeris {
    ThirdBodyDataInfo info_{"synthetic",
                            std::string(64, 'a'),
                            "test-v1",
                            Epoch{-172800000000000},
                            Epoch{172800000000000},
                            1.3271244004127942e20,
                            4.9028001184575496e12};

  public:
    mutable std::size_t sun_calls = 0, moon_calls = 0;
    const ThirdBodyDataInfo &data_info() const noexcept override { return info_; }
    void validate_interval(Epoch first, Epoch last, bool, bool) const override {
        if (first.tai_nanoseconds() < info_.first.tai_nanoseconds() ||
            last.tai_nanoseconds() > info_.last.tai_nanoseconds())
            throw std::out_of_range("Synthetic coverage exceeded");
    }
    std::array<double, 3> position(ThirdBody body, Epoch epoch) const override {
        const bool sun = body == ThirdBody::sun;
        ++(sun ? sun_calls : moon_calls);
        const double radius = sun ? 1.496e11 : 384400000;
        const double rate = 2 * std::numbers::pi / ((sun ? 365.25 : 27.321661) * 86400);
        const double tilt = sun ? .409 : .5;
        const double angle = (sun ? .3 : 1.2) + rate * epoch.seconds_since(Epoch{0});
        return {radius * std::cos(angle), radius * std::sin(angle) * std::cos(tilt),
                radius * std::sin(angle) * std::sin(tilt)};
    }
};
} // namespace
TEST(ThirdBody, StableTidalLimitSymmetryAndInvalidInputs) {
    constexpr double mu = 1.3271244004127942e20, distance = 1.496e11;
    // At one millimetre from the origin the direct subtraction loses useful digits.
    const auto a = third_body_acceleration({.001, 0, 0}, {distance, 0, 0}, mu);
    EXPECT_NEAR(a[0], 2 * mu * .001 / std::pow(distance, 3), 1e-30);
    EXPECT_EQ(a[1], 0);
    EXPECT_EQ(third_body_acceleration({0, 0, 0}, {distance, 0, 0}, mu), (std::array{0., 0., 0.}));
    const auto normal = third_body_acceleration({7e6, 2e6, -3e6}, {3e8, -1e8, 2e8}, 4.9e12);
    const auto mirrored = third_body_acceleration({-7e6, -2e6, 3e6}, {-3e8, 1e8, -2e8}, 4.9e12);
    for (int i = 0; i < 3; ++i)
        EXPECT_EQ(normal[i], -mirrored[i]);
    EXPECT_THROW((void)third_body_acceleration({1, 0, 0}, {0, 0, 0}, mu), std::invalid_argument);
    EXPECT_THROW((void)third_body_acceleration({1, 0, 0}, {1, 0, 0}, mu), std::invalid_argument);
    EXPECT_THROW((void)third_body_acceleration({1, 0, 0}, {2, 0, 0}, -1), std::invalid_argument);
    EXPECT_THROW((void)third_body_acceleration({NAN, 0, 0}, {2, 0, 0}, mu), std::invalid_argument);
}
TEST(ThirdBody, MovingSunAndMoonMatchIndependentOrekitIntegration) {
    std::ifstream source(ORBITFORGE_THIRD_BODY_REFERENCE);
    ASSERT_TRUE(source);
    std::string line;
    std::size_t cases = 0;
    while (std::getline(source, line)) {
        if (line.empty() || line[0] == '#')
            continue;
        std::istringstream row(line);
        int mode = 0;
        double seconds = 0;
        CartesianState initial{Epoch{0}, OriginId::earth, FrameId::gcrf, {}, {}},
            expected = initial;
        std::array<double, 3> acceleration{};
        ASSERT_TRUE(row >> mode >> seconds);
        for (auto &v : initial.position_m)
            ASSERT_TRUE(row >> v);
        for (auto &v : initial.velocity_mps)
            ASSERT_TRUE(row >> v);
        for (auto &v : expected.position_m)
            ASSERT_TRUE(row >> v);
        for (auto &v : expected.velocity_mps)
            ASSERT_TRUE(row >> v);
        for (auto &v : acceleration)
            ASSERT_TRUE(row >> v);
        HpopOptions options;
        options.gravity_degree = options.gravity_order = 0;
        options.sun_gravity = mode != 1;
        options.moon_gravity = mode != 0;
        options.relative_tolerance = 1e-13;
        options.position_tolerance_m = 1e-5;
        options.velocity_tolerance_mps = 1e-8;
        auto bodies = std::make_shared<MovingBodies>();
        const auto &info = bodies->data_info();
        if (mode == 3) {
            options.sun_mu = info.sun_mu * 1.01;
            options.moon_mu = info.moon_mu * .99;
        }
        std::array<double, 3> actual_force{};
        for (auto body : {ThirdBody::sun, ThirdBody::moon}) {
            if (body == ThirdBody::sun ? !options.sun_gravity : !options.moon_gravity)
                continue;
            const auto mu = body == ThirdBody::sun ? options.sun_mu.value_or(info.sun_mu)
                                                   : options.moon_mu.value_or(info.moon_mu);
            const auto force = third_body_acceleration(initial.position_m,
                                                       bodies->position(body, initial.epoch), mu);
            for (int i = 0; i < 3; ++i)
                actual_force[i] += force[i];
        }
        for (int i = 0; i < 3; ++i)
            EXPECT_NEAR(actual_force[i], acceleration[i], 3e-18);
        const auto provider = make_hpop(initial, options, {nullptr, nullptr, nullptr, bodies});
        expected.epoch = Epoch{static_cast<std::int64_t>(seconds * 1e9)};
        const auto result = provider->state_at(expected.epoch);
        SCOPED_TRACE(line);
        for (int i = 0; i < 3; ++i) {
            EXPECT_NEAR(result.position_m[i], expected.position_m[i], .002);
            EXPECT_NEAR(result.velocity_mps[i], expected.velocity_mps[i], 2e-6);
        }
        EXPECT_EQ(provider->state_at(expected.epoch).position_m, result.position_m);
        EXPECT_EQ(bodies->sun_calls > 1, options.sun_gravity);
        EXPECT_EQ(bodies->moon_calls > 1, options.moon_gravity);
        ++cases;
    }
    EXPECT_EQ(cases, 32U);
}
TEST(ThirdBody, ResourceCoverageAndCancellationAreEnforcedBeforePublishingResults) {
    HpopOptions options;
    options.gravity_degree = options.gravity_order = 0;
    options.sun_gravity = true;
    const CartesianState initial{
        Epoch{0}, OriginId::earth, FrameId::gcrf, {7e6, 0, 0}, {0, 6200, 5300}};
    EXPECT_THROW((void)make_hpop(initial, options), std::invalid_argument);
    auto bodies = std::make_shared<MovingBodies>();
    const auto provider = make_hpop(initial, options, {nullptr, nullptr, nullptr, bodies});
    const auto calls = bodies->sun_calls;
    EXPECT_THROW((void)provider->state_at(Epoch{172800000000001}), std::out_of_range);
    EXPECT_EQ(calls, bodies->sun_calls);
    int checks = 0;
    EXPECT_THROW((void)provider->state_at(Epoch{600000000000},
                                          [&] {
                                              if (++checks == 5)
                                                  throw std::runtime_error("cancel");
                                          }),
                 std::runtime_error);
    EXPECT_EQ(provider->state_at(Epoch{600000000000}).position_m,
              make_hpop(initial, options, {nullptr, nullptr, nullptr, bodies})
                  ->state_at(Epoch{600000000000})
                  .position_m);
    options.moon_mu = 0;
    EXPECT_THROW((void)validate_hpop_options(options), std::invalid_argument);
}
