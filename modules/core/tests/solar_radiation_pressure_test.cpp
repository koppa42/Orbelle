#include <cmath>
#include <fstream>
#include <gtest/gtest.h>
#include <iostream>
#include <limits>
#include <numbers>
#include <orbitforge/core/ephemeris/ephemeris_provider.hpp>
#include <orbitforge/core/propagation/hpop.hpp>
#include <sstream>

using namespace orbitforge::core;
namespace {
constexpr double au = 149597870700.0;
std::array<double, 3> plus(std::array<double, 3> a, const std::array<double, 3> &b) {
    for (int i = 0; i < 3; ++i)
        a[i] += b[i];
    return a;
}
std::array<double, 3> direction(double x, double y) {
    const double distance = std::hypot(x, y);
    if (distance == 0)
        return {1, 0, 0};
    return {std::cos(distance), -std::sin(distance) * x / distance,
            -std::sin(distance) * y / distance};
}
std::array<double, 3> scale(std::array<double, 3> v, double factor) {
    for (auto &component : v)
        component *= factor;
    return v;
}
double disk_ratio(double earth_x, double earth_y, double earth_radius, double moon_x = 0,
                  double moon_y = 0, double moon_radius = 0) {
    // Coordinates/radii are in units of the apparent solar radius.
    constexpr double solar_angle = .005, earth_distance = 7e6, moon_distance = 1e8;
    const auto satellite =
        scale(direction(earth_x * solar_angle, earth_y * solar_angle), -earth_distance);
    const auto sun = plus(satellite, {au, 0, 0});
    const auto moon = plus(
        satellite, scale(direction(moon_x * solar_angle, moon_y * solar_angle), moon_distance));
    SrpOptions options;
    options.enabled = true;
    options.sun_radius_m = au * std::sin(solar_angle);
    options.earth_shadow = earth_radius > 0;
    options.moon_shadow = moon_radius > 0;
    options.earth_radius_m = earth_distance * std::sin(std::max(earth_radius, 1e-4) * solar_angle);
    options.moon_radius_m = moon_distance * std::sin(std::max(moon_radius, 1e-4) * solar_angle);
    return evaluate_srp(satellite, sun, options, moon).lighting_ratio;
}
class MovingSolarSystem final : public IThirdBodyEphemeris {
    ThirdBodyDataInfo info_{"synthetic-srp",         std::string(64, 'a'),   "test-v1",
                            Epoch{-172800000000000}, Epoch{172800000000000}, 1.3271244004127942e20,
                            4.9028001184575496e12};

  public:
    bool moon_available = true;
    mutable std::size_t sun_calls = 0, moon_calls = 0;
    const ThirdBodyDataInfo &data_info() const noexcept override { return info_; }
    void validate_interval(Epoch first, Epoch last,
                           std::span<const ThirdBody> bodies) const override {
        if (first.tai_nanoseconds() < info_.first.tai_nanoseconds() ||
            last.tai_nanoseconds() > info_.last.tai_nanoseconds())
            throw EphemerisCoverageError("Synthetic SRP interval is not covered");
        for (auto body : bodies)
            if (body == ThirdBody::moon && !moon_available)
                throw EphemerisCoverageError("Moon is not covered");
    }
    std::array<double, 3> position(ThirdBody body, Epoch epoch) const override {
        const bool sun = body == ThirdBody::sun;
        ++(sun ? sun_calls : moon_calls);
        const double radius = sun ? au : 384400000;
        const double rate = 2 * std::numbers::pi / ((sun ? 365.25 : 27.321661) * 86400);
        const double tilt = sun ? .409 : .5;
        const double angle = (sun ? .03 : 1.2) + rate * epoch.seconds_since(Epoch{0});
        return {radius * std::cos(angle), radius * std::sin(angle) * std::cos(tilt),
                radius * std::sin(angle) * std::sin(tilt)};
    }
};
} // namespace
TEST(SolarRadiationPressure, DirectionReferencePressureInverseSquareAndSignedCoefficient) {
    SrpOptions options;
    options.enabled = true;
    options.shadow_model = SrpShadowModel::none;
    const auto original = evaluate_srp({0, 0, 0}, {au, 0, 0}, options);
    EXPECT_EQ(original.lighting_ratio, 1);
    EXPECT_NEAR(original.acceleration_mps2[0], -4.56e-6 * .02, 1e-22);
    EXPECT_EQ(original.acceleration_mps2[1], 0);
    EXPECT_EQ(original.acceleration_mps2[2], 0);
    const auto farther = evaluate_srp({0, 0, 0}, {2 * au, 0, 0}, options);
    EXPECT_EQ(farther.acceleration_mps2[0], original.acceleration_mps2[0] / 4);
    options.cr = -2;
    EXPECT_EQ(evaluate_srp({0, 0, 0}, {au, 0, 0}, options).acceleration_mps2[0],
              -2 * original.acceleration_mps2[0]);
    options.area_mass_ratio_m2_per_kg = 0;
    EXPECT_EQ(evaluate_srp({0, 0, 0}, {au, 0, 0}, options).acceleration_mps2,
              (std::array{0., 0., 0.}));
}
TEST(SolarRadiationPressure, CylindricalShadowUsesRayDirectionAndAtmosphericRadius) {
    SrpOptions options;
    options.enabled = true;
    options.moon_shadow = false;
    options.shadow_model = SrpShadowModel::cylindrical;
    EXPECT_EQ(evaluate_srp({-7e6, 0, 0}, {au, 0, 0}, options).lighting_ratio, 0);
    EXPECT_EQ(evaluate_srp({7e6, 0, 0}, {au, 0, 0}, options).lighting_ratio, 1);
    EXPECT_EQ(evaluate_srp({-7e6, 7e6, 0}, {au, 0, 0}, options).lighting_ratio, 1);
    const std::array satellite{-7e6, options.earth_radius_m + 10000, 0.};
    const auto sun = plus(satellite, {au, 0, 0});
    EXPECT_EQ(evaluate_srp(satellite, sun, options).lighting_ratio, 1);
    options.atmospheric_altitude_m = 23000;
    EXPECT_EQ(evaluate_srp(satellite, sun, options).lighting_ratio, 0);
}
TEST(SolarRadiationPressure, DualConeHandlesPenumbraUmbraAnnularAndTangency) {
    EXPECT_EQ(disk_ratio(0, 0, 2), 0);
    EXPECT_EQ(disk_ratio(3, 0, 1), 1);
    EXPECT_NEAR(disk_ratio(0, 0, .5), .75, 2e-12);
    EXPECT_NEAR(disk_ratio(-.5, 0, .5), .75, 2e-11);
    EXPECT_NEAR(disk_ratio(.5, 0, .5), .75, 2e-11);
    const double overlap = 2 * std::acos(.5) - std::sqrt(3.0) / 2;
    EXPECT_NEAR(disk_ratio(1, 0, 1), 1 - overlap / std::numbers::pi, 2e-11);
    EXPECT_NEAR(disk_ratio(2, 0, 1), 1, 2e-12);
    EXPECT_NEAR(disk_ratio(1 - 1e-8, 0, 2), 0, 2e-12);
}
TEST(SolarRadiationPressure, MoonAndEarthUseDiskUnionIncludingCoincidentAndNestedOccultations) {
    EXPECT_NEAR(disk_ratio(.5, 0, .25, -.5, 0, .25), .875, 2e-11);
    EXPECT_NEAR(disk_ratio(0, 0, .5, 0, 0, .5), .75, 2e-11);
    EXPECT_NEAR(disk_ratio(0, 0, .8, 0, 0, .2), .36, 2e-11);
    EXPECT_NEAR(disk_ratio(.25, 0, .25, -.25, 0, .25), .875, 2e-11);
    EXPECT_EQ(disk_ratio(0, 0, 2, -.5, 0, .25), 0);
}
TEST(SolarRadiationPressure, InvalidConfigurationAndRequiredGeometryFailExplicitly) {
    SrpOptions options;
    options.enabled = true;
    EXPECT_THROW((void)evaluate_srp({7e6, 0, 0}, {au, 0, 0}, options), std::invalid_argument);
    options.moon_shadow = false;
    EXPECT_THROW((void)evaluate_srp({0, 0, 0}, {au, 0, 0}, options), PropagationError);
    EXPECT_THROW((void)evaluate_srp({7e6, 0, 0}, {7e6, 0, 0}, options), PropagationError);
    EXPECT_THROW((void)evaluate_srp({NAN, 0, 0}, {au, 0, 0}, options), PropagationError);
    for (double value : {-101., 101., std::numeric_limits<double>::quiet_NaN()}) {
        options.cr = value;
        EXPECT_THROW(validate_srp_options(options), std::invalid_argument);
    }
    options = {};
    EXPECT_EQ(evaluate_srp({}, {}, options).acceleration_mps2, (std::array{0., 0., 0.}));
    options.pressure_at_reference_pa = 0;
    EXPECT_THROW(validate_srp_options(options), std::invalid_argument);
    options = {};
    options.area_mass_ratio_m2_per_kg = -1;
    EXPECT_THROW(validate_srp_options(options), std::invalid_argument);
    options.shadow_model = static_cast<SrpShadowModel>(99);
    EXPECT_THROW(validate_srp_options(options), std::invalid_argument);
}
TEST(SolarRadiationPressure, HpopRequiresSunWithoutThirdBodyGravityAndOnlyRequiredMoonCoverage) {
    HpopOptions options;
    options.gravity_degree = options.gravity_order = 0;
    options.srp.enabled = true;
    options.srp.shadow_model = SrpShadowModel::none;
    options.srp.area_mass_ratio_m2_per_kg = 1000;
    options.max_step_seconds = 1;
    options.position_tolerance_m = 1e-8;
    options.velocity_tolerance_mps = 1e-10;
    const CartesianState initial{Epoch{0}, OriginId::earth, FrameId::gcrf, {7e6, 0, 0}, {0, 0, 0}};
    EXPECT_FALSE(options.uses_third_bodies());
    EXPECT_TRUE(options.uses_ephemeris());
    EXPECT_EQ(options.required_ephemeris_bodies(), std::vector{ThirdBody::sun});
    EXPECT_THROW((void)make_hpop(initial, options), std::invalid_argument);
    auto bodies = std::make_shared<MovingSolarSystem>();
    bodies->moon_available = false;
    HpopEnvironment environment;
    environment.ephemeris = bodies;
    auto provider = make_hpop(initial, options, environment);
    const auto affected = provider->state_at(Epoch{1000000000});
    const double ax =
        evaluate_srp(initial.position_m, bodies->position(ThirdBody::sun, Epoch{0}), options.srp)
            .acceleration_mps2[0];
    options.srp.enabled = false;
    const auto baseline = make_hpop(initial, options)->state_at(Epoch{1000000000});
    EXPECT_NEAR(affected.position_m[0] - baseline.position_m[0], ax / 2, 2e-8);
    EXPECT_GT(bodies->sun_calls, 13U);
    EXPECT_EQ(bodies->moon_calls, 0U);
    options.srp.enabled = true;
    options.srp.shadow_model = SrpShadowModel::dual_cone;
    EXPECT_THROW((void)make_hpop(initial, options, environment), EphemerisCoverageError);
    EXPECT_THROW((void)provider->state_at(Epoch{172801000000000}), EphemerisCoverageError);
}
TEST(SolarRadiationPressure, CancellationDuringSrpIntegrationRetainsReusableCheckpoints) {
    const auto bodies = std::make_shared<MovingSolarSystem>();
    const CartesianState initial{
        Epoch{0}, OriginId::earth, FrameId::gcrf, {7e6, 0, 0}, {0, 6200, 5300}};
    HpopOptions options;
    options.gravity_degree = options.gravity_order = 0;
    options.srp.enabled = true;
    const HpopEnvironment environment{nullptr, nullptr, nullptr, bodies};
    const auto provider = make_hpop(initial, options, environment);
    const auto target = Epoch{6000000000000};
    unsigned checkpoints = 0;
    EXPECT_THROW((void)provider->state_at(target,
                                          [&] {
                                              if (++checkpoints == 40)
                                                  throw std::runtime_error("cancel-srp");
                                          }),
                 std::runtime_error);
    EXPECT_GT(bodies->sun_calls, 0U);
    EXPECT_GT(bodies->moon_calls, 0U);
    const auto expected = make_hpop(initial, options, environment)->state_at(target);
    EXPECT_EQ(provider->state_at(target).position_m, expected.position_m);
    EXPECT_EQ(provider->state_at(target).velocity_mps, expected.velocity_mps);
}

TEST(SolarRadiationPressure, MovingSunAndEarthEclipsesMatchIndependentOrekitTrajectories) {
    std::ifstream source(ORBITFORGE_SRP_REFERENCE);
    ASSERT_TRUE(source);
    std::string line;
    std::size_t count = 0;
    double max_position = 0, max_velocity = 0;
    while (std::getline(source, line)) {
        if (line.empty() || line[0] == '#')
            continue;
        SCOPED_TRACE(line);
        std::istringstream row(line);
        int mode;
        double seconds;
        CartesianState initial{Epoch{0}, OriginId::earth, FrameId::gcrf, {}, {}},
            expected = initial;
        std::array<double, 3> acceleration;
        double sun_radius;
        ASSERT_TRUE(row >> mode >> seconds);
        for (auto &value : initial.position_m)
            ASSERT_TRUE(row >> value);
        for (auto &value : initial.velocity_mps)
            ASSERT_TRUE(row >> value);
        for (auto &value : expected.position_m)
            ASSERT_TRUE(row >> value);
        for (auto &value : expected.velocity_mps)
            ASSERT_TRUE(row >> value);
        for (auto &value : acceleration)
            ASSERT_TRUE(row >> value);
        ASSERT_TRUE(row >> sun_radius);
        HpopOptions options;
        options.gravity_degree = options.gravity_order = 0;
        options.relative_tolerance = 1e-14;
        options.position_tolerance_m = 1e-8;
        options.velocity_tolerance_mps = 1e-11;
        options.min_step_seconds = 1e-6;
        options.max_step_seconds = 2.5;
        options.srp.enabled = true;
        options.srp.moon_shadow = false;
        options.srp.sun_radius_m = sun_radius;
        options.srp.atmospheric_altitude_m = mode == 1 ? 23000 : 0;
        options.srp.cr = mode == 2 ? 1.7 : 1;
        options.srp.area_mass_ratio_m2_per_kg = mode == 2 ? .025 : .02;
        auto bodies = std::make_shared<MovingSolarSystem>();
        HpopEnvironment environment;
        environment.ephemeris = bodies;
        const auto a = evaluate_srp(initial.position_m, bodies->position(ThirdBody::sun, Epoch{0}),
                                    options.srp);
        for (int i = 0; i < 3; ++i)
            EXPECT_NEAR(a.acceleration_mps2[i], acceleration[i], 1e-20);
        const auto actual = make_hpop(initial, options, environment)
                                ->state_at(Epoch{static_cast<std::int64_t>(seconds * 1e9)});
        for (int i = 0; i < 3; ++i) {
            const double position_error = std::abs(actual.position_m[i] - expected.position_m[i]);
            const double velocity_error =
                std::abs(actual.velocity_mps[i] - expected.velocity_mps[i]);
            max_position = std::max(max_position, position_error);
            max_velocity = std::max(max_velocity, velocity_error);
            EXPECT_NEAR(actual.position_m[i], expected.position_m[i], .0005);
            EXPECT_NEAR(actual.velocity_mps[i], expected.velocity_mps[i], 2e-7);
        }
        ++count;
    }
    EXPECT_EQ(count, 24U);
    std::cout << "SRP Orekit reference: max component position " << max_position << " m; velocity "
              << max_velocity << " m/s\n";
}
TEST(SolarRadiationPressure, ClippedEarthMoonDiskUnionMatchesIndependentNumericalQuadrature) {
    std::ifstream source(ORBITFORGE_SRP_GEOMETRY_REFERENCE);
    ASSERT_TRUE(source);
    std::string line;
    std::size_t count = 0;
    double maximum = 0;
    while (std::getline(source, line)) {
        if (line.empty() || line[0] == '#')
            continue;
        SCOPED_TRACE(line);
        std::istringstream row(line);
        std::array<double, 6> disks;
        for (auto &value : disks)
            ASSERT_TRUE(row >> value);
        double expected;
        ASSERT_TRUE(row >> expected);
        const auto actual = disk_ratio(disks[0], disks[1], disks[2], disks[3], disks[4], disks[5]);
        maximum = std::max(maximum, std::abs(actual - expected));
        EXPECT_NEAR(actual, expected, 2e-9);
        ++count;
    }
    EXPECT_EQ(count, 182U);
    std::cout << "SRP clipped disk-union reference: maximum fraction error " << maximum << '\n';
}
