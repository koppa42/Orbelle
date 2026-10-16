#include <algorithm>
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
    options.srp.sun_position_method = SrpSunPositionMethod::geometric;
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
    options.srp.sun_position_method = SrpSunPositionMethod::geometric;
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
    options.srp.sun_position_method = SrpSunPositionMethod::geometric;
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
        options.srp.sun_position_method = SrpSunPositionMethod::geometric;
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

namespace {
class ApparentSolarSystem final : public IThirdBodyEphemeris {
    ThirdBodyDataInfo info_{"apparent-srp",        std::string(64, 'b'), "test-v1",
                            Epoch{-1000000000000}, Epoch{1000000000000}, 1.3271244004127942e20,
                            4.9028001184575496e12};

  public:
    mutable unsigned geometric = 0, earth = 0, spacecraft = 0;
    mutable unsigned earth_checks = 0, spacecraft_checks = 0;
    mutable std::array<double, 3> last_position{}, last_velocity{};
    const ThirdBodyDataInfo &data_info() const noexcept override { return info_; }
    void validate_interval(Epoch, Epoch, std::span<const ThirdBody>) const override {}
    void validate_apparent_sun_interval(Epoch, Epoch) const override { ++earth_checks; }
    void validate_spacecraft_apparent_sun_interval(Epoch, Epoch) const override {
        ++spacecraft_checks;
    }
    std::array<double, 3> position(ThirdBody, Epoch) const override {
        ++geometric;
        return {au, 0, 0};
    }
    std::array<double, 3> apparent_sun_from_earth(Epoch) const override {
        ++earth;
        return {au, 1e8, 0};
    }
    std::array<double, 3>
    apparent_sun_from_spacecraft(Epoch, const std::array<double, 3> &r,
                                 const std::array<double, 3> &v) const override {
        ++spacecraft;
        last_position = r;
        last_velocity = v;
        return {au - r[0], 2e8 - r[1] + 1000 * v[1], -r[2] + 1000 * v[2]};
    }
};
} // namespace
TEST(SolarRadiationPressure, SunMethodsUseStageObserversAndKeepGravityGeometric) {
    const CartesianState initial{
        Epoch{0}, OriginId::earth, FrameId::gcrf, {7e6, 0, 0}, {0, 6200, 5300}};
    HpopOptions options;
    options.gravity_degree = options.gravity_order = 0;
    options.srp.enabled = true;
    options.srp.shadow_model = SrpShadowModel::none;
    options.srp.area_mass_ratio_m2_per_kg = 1000;
    std::array<std::array<double, 3>, 3> results;
    int index = 0;
    for (auto method : {SrpSunPositionMethod::geometric, SrpSunPositionMethod::apparent_to_true_cb,
                        SrpSunPositionMethod::apparent}) {
        SCOPED_TRACE(index);
        options.srp.sun_position_method = method;
        const auto bodies = std::make_shared<ApparentSolarSystem>();
        const HpopEnvironment env{nullptr, nullptr, nullptr, bodies};
        const auto provider = make_hpop(initial, options, env);
        results[index++] = provider->state_at(Epoch{100000000000}).position_m;
        (void)provider->state_at(Epoch{-100000000000});
        EXPECT_EQ(bodies->geometric > 0, method == SrpSunPositionMethod::geometric);
        EXPECT_EQ(bodies->earth > 0, method == SrpSunPositionMethod::apparent_to_true_cb);
        EXPECT_EQ(bodies->spacecraft > 0, method == SrpSunPositionMethod::apparent);
        EXPECT_EQ(bodies->earth_checks > 0, method == SrpSunPositionMethod::apparent_to_true_cb);
        EXPECT_EQ(bodies->spacecraft_checks > 0, method == SrpSunPositionMethod::apparent);
        if (method == SrpSunPositionMethod::apparent) {
            EXPECT_NE(bodies->last_position, initial.position_m);
            EXPECT_NE(bodies->last_velocity, initial.velocity_mps);
        }
    }
    EXPECT_NE(results[0], results[1]);
    EXPECT_NE(results[1], results[2]);
    options.third_body(ThirdBody::sun).enabled = true;
    const auto bodies = std::make_shared<ApparentSolarSystem>();
    const HpopEnvironment env{nullptr, nullptr, nullptr, bodies};
    (void)make_hpop(initial, options, env)->state_at(Epoch{100000000000});
    EXPECT_GT(bodies->spacecraft, 1U);
    EXPECT_EQ(bodies->geometric, bodies->spacecraft); // One separate gravity query at each stage.
}
TEST(SolarRadiationPressure, ApparentModesRequireCapableProvidersAndValidateEnums) {
    const CartesianState initial{
        Epoch{0}, OriginId::earth, FrameId::gcrf, {7e6, 0, 0}, {0, 6200, 5300}};
    HpopOptions options;
    options.gravity_degree = options.gravity_order = 0;
    options.srp.enabled = true;
    const HpopEnvironment env{nullptr, nullptr, nullptr, std::make_shared<MovingSolarSystem>()};
    EXPECT_EQ(options.srp.sun_position_method, SrpSunPositionMethod::apparent_to_true_cb);
    EXPECT_THROW((void)make_hpop(initial, options, env), std::invalid_argument);
    options.srp.sun_position_method = SrpSunPositionMethod::apparent;
    EXPECT_THROW((void)make_hpop(initial, options, env), std::invalid_argument);
    options.srp.sun_position_method = SrpSunPositionMethod::geometric;
    EXPECT_NO_THROW((void)make_hpop(initial, options, env));
    options.srp.enabled = false;
    options.srp.sun_position_method = static_cast<SrpSunPositionMethod>(99);
    EXPECT_THROW(validate_srp_options(options.srp), std::invalid_argument);
}

TEST(SolarRadiationPressure, UpToTenOccultorsMatchIndependentScipyQuadpackAndAreOrderIndependent) {
    std::ifstream input(ORBITFORGE_SRP_MULTI_REFERENCE);
    ASSERT_TRUE(input);
    std::string line;
    unsigned cases = 0;
    double maximum = 0;
    while (std::getline(input, line)) {
        if (line.empty() || line[0] == '#')
            continue;
        SCOPED_TRACE(line);
        std::istringstream row(line);
        std::size_t count;
        double expected;
        ASSERT_TRUE(row >> count >> expected);
        ASSERT_LE(count, 10U);
        std::vector<std::array<double, 3>> disks(count);
        for (auto &disk : disks)
            ASSERT_TRUE(row >> disk[0] >> disk[1] >> disk[2]);
        constexpr double alpha = .001;
        SrpOptions options;
        options.enabled = true;
        options.sun_radius_m = au * std::sin(alpha);
        options.earth_shadow = count > 0;
        options.moon_shadow = count > 1;
        std::array<double, 3> satellite{};
        if (count) {
            satellite = scale(direction(disks[0][0] * alpha, disks[0][1] * alpha), -7e6);
            options.earth_radius_m = 7e6 * std::sin(disks[0][2] * alpha);
        }
        std::optional<std::array<double, 3>> moon;
        std::vector<SrpBodyPosition> additional;
        for (std::size_t i = 1; i < count; ++i) {
            const double distance = 1e8 * static_cast<double>(i);
            const auto position = plus(
                satellite, scale(direction(disks[i][0] * alpha, disks[i][1] * alpha), distance));
            const double radius = distance * std::sin(disks[i][2] * alpha);
            if (i == 1) {
                moon = position;
                options.moon_radius_m = radius;
            } else {
                const auto body = third_body_list[i];
                options.additional_eclipsing_bodies.push_back({body, radius});
                additional.push_back({body, position});
            }
        }
        const auto sun = plus(satellite, {au, 0, 0});
        const auto actual = evaluate_srp(satellite, sun, options, moon, additional);
        maximum = std::max(maximum, std::abs(actual.lighting_ratio - expected));
        EXPECT_NEAR(actual.lighting_ratio, expected, 2e-9);
        std::ranges::reverse(options.additional_eclipsing_bodies);
        std::ranges::reverse(additional);
        const auto reversed = evaluate_srp(satellite, sun, options, moon, additional);
        EXPECT_EQ(reversed.lighting_ratio, actual.lighting_ratio);
        EXPECT_EQ(reversed.acceleration_mps2, actual.acceleration_mps2);
        ++cases;
    }
    EXPECT_EQ(cases, 175U);
    RecordProperty("max_multi_occultor_fraction_error", maximum);
}
TEST(SolarRadiationPressure, PlanetAssignmentsValidateBodyCentresRadiiAndRequiredPositions) {
    SrpOptions options;
    options.enabled = true;
    options.earth_shadow = options.moon_shadow = false;
    for (auto body : {ThirdBody::sun, ThirdBody::moon, static_cast<ThirdBody>(-1),
                      static_cast<ThirdBody>(10)}) {
        options.additional_eclipsing_bodies = {{body, 1}};
        EXPECT_THROW(validate_srp_options(options), std::invalid_argument);
        EXPECT_THROW((void)default_srp_eclipsing_body(body), std::invalid_argument);
    }
    options.additional_eclipsing_bodies = {{ThirdBody::mars, 1e6}, {ThirdBody::mars, 1e6}};
    EXPECT_THROW(validate_srp_options(options), std::invalid_argument);
    for (double radius : {0., -1., std::numeric_limits<double>::infinity(),
                          std::numeric_limits<double>::quiet_NaN()}) {
        options.additional_eclipsing_bodies = {{ThirdBody::mars, radius}};
        EXPECT_THROW(validate_srp_options(options), std::invalid_argument);
    }
    options.additional_eclipsing_bodies = {{ThirdBody::mars, 1e6}, {ThirdBody::jupiter, 1e6}};
    const std::array<double, 3> observer{}, sun{au, 0, 0};
    std::vector<SrpBodyPosition> positions{{ThirdBody::mars, {1e8, 0, 0}},
                                           {ThirdBody::jupiter, {2e8, 0, 0}}};
    EXPECT_EQ(evaluate_srp(observer, sun, options, {}, positions).lighting_ratio, 0);
    EXPECT_THROW((void)evaluate_srp(observer, sun, options), std::invalid_argument);
    positions[1].body = ThirdBody::mars;
    EXPECT_THROW((void)evaluate_srp(observer, sun, options, {}, positions), std::invalid_argument);
    positions[1].body = ThirdBody::venus;
    EXPECT_THROW((void)evaluate_srp(observer, sun, options, {}, positions), std::invalid_argument);
    positions[1].body = ThirdBody::jupiter;
    positions[1].position_m = {0, 0, 0};
    options.shadow_model = SrpShadowModel::cylindrical;
    EXPECT_THROW((void)evaluate_srp(observer, sun, options, {}, positions), PropagationError);
    options.shadow_model = SrpShadowModel::none;
    EXPECT_EQ(evaluate_srp(observer, sun, options, {}, positions).lighting_ratio, 1);
    EXPECT_TRUE(effective_srp_options(options).additional_eclipsing_bodies.empty());
    options.enabled = false;
    EXPECT_TRUE(effective_srp_options(options).additional_eclipsing_bodies.empty());
    EXPECT_EQ(srp_eclipsing_body_naif_id(ThirdBody::jupiter), 599);
    EXPECT_EQ(third_body_naif_id(ThirdBody::jupiter), 5);
    EXPECT_EQ(default_srp_eclipsing_body(ThirdBody::mercury).radius_m, 2440530);
}
namespace {
class TransitingPlanets final : public IThirdBodyEphemeris {
    ThirdBodyDataInfo info_{"synthetic-planets",    std::string(64, 'c'),  "test-v1",
                            Epoch{-10000000000000}, Epoch{10000000000000}, 1.3271244004127942e20,
                            4.9028001184575496e12};

  public:
    mutable unsigned positions = 0, certified = 0;
    const ThirdBodyDataInfo &data_info() const noexcept override { return info_; }
    void validate_interval(Epoch a, Epoch b, std::span<const ThirdBody> bodies) const override {
        if (a.tai_nanoseconds() < info_.first.tai_nanoseconds() ||
            b.tai_nanoseconds() > info_.last.tai_nanoseconds())
            throw EphemerisCoverageError("Synthetic planet coverage exceeded");
        for (auto body : bodies)
            if (body != ThirdBody::sun)
                throw std::invalid_argument("No barycentre force requested");
    }
    std::array<double, 3> position(ThirdBody body, Epoch) const override {
        if (body != ThirdBody::sun)
            throw std::invalid_argument("Silhouette used a gravity barycentre");
        return {au, 0, 0};
    }
    void validate_eclipsing_body_interval(Epoch a, Epoch b,
                                          std::span<const ThirdBody> bodies) const override {
        validate_interval(a, b, {});
        ++certified;
        for (auto body : bodies)
            if (body != ThirdBody::mercury && body != ThirdBody::venus &&
                body != ThirdBody::jupiter)
                throw EphemerisCoverageError("Missing synthetic body centre");
    }
    std::array<double, 3> eclipsing_body_position(ThirdBody body, Epoch at) const override {
        ++positions;
        const double t = at.seconds_since(Epoch{0});
        if (body == ThirdBody::mercury)
            return {3e10, 9e7 * std::sin(t / 3600 + .1), 2e7};
        if (body == ThirdBody::venus)
            return {4e10, -1.4e8 + 30000 * t, -3e7};
        if (body == ThirdBody::jupiter)
            return {5e10, 1e8 * std::sin(t / 2400 + .5), 1.1e8};
        throw EphemerisCoverageError("Missing synthetic body centre");
    }
};
} // namespace
TEST(SolarRadiationPressure, MovingPlanetUnionsMatchIndependentScipyDop853AndQuadpackPropagation) {
    std::ifstream input(ORBITFORGE_SRP_MULTI_TRAJECTORIES);
    ASSERT_TRUE(input);
    const auto planets = std::make_shared<TransitingPlanets>();
    HpopOptions options;
    options.gravity_degree = options.gravity_order = 0;
    options.max_step_seconds = 2.5;
    options.relative_tolerance = 1e-14;
    options.position_tolerance_m = 1e-8;
    options.velocity_tolerance_mps = 1e-11;
    options.srp.enabled = true;
    options.srp.sun_position_method = SrpSunPositionMethod::geometric;
    options.srp.moon_shadow = false;
    options.srp.cr = 1.7;
    options.srp.area_mass_ratio_m2_per_kg = .2;
    options.srp.additional_eclipsing_bodies = {
        {ThirdBody::mercury, 9e7}, {ThirdBody::venus, 1.25e8}, {ThirdBody::jupiter, 8e7}};
    HpopEnvironment environment;
    environment.ephemeris = planets;
    std::string line;
    unsigned cases = 0;
    double max_position = 0, max_velocity = 0;
    while (std::getline(input, line)) {
        if (line.empty() || line[0] == '#')
            continue;
        SCOPED_TRACE(line);
        std::istringstream row(line);
        CartesianState initial{Epoch{0}, OriginId::earth, FrameId::gcrf, {}, {}};
        for (auto &v : initial.position_m)
            ASSERT_TRUE(row >> v);
        for (auto &v : initial.velocity_mps)
            ASSERT_TRUE(row >> v);
        double elapsed;
        ASSERT_TRUE(row >> elapsed);
        std::array<double, 6> reference;
        for (auto &v : reference)
            ASSERT_TRUE(row >> v);
        const auto result = make_hpop(initial, options, environment)
                                ->state_at(Epoch{static_cast<std::int64_t>(elapsed * 1e9)});
        for (unsigned i = 0; i < 3; ++i) {
            max_position = std::max(max_position, std::abs(result.position_m[i] - reference[i]));
            max_velocity =
                std::max(max_velocity, std::abs(result.velocity_mps[i] - reference[i + 3]));
            EXPECT_NEAR(result.position_m[i], reference[i], .0005);
            EXPECT_NEAR(result.velocity_mps[i], reference[i + 3], 2e-7);
        }
        ++cases;
    }
    EXPECT_EQ(cases, 4U);
    EXPECT_GT(planets->positions, 1000U);
    EXPECT_GT(planets->certified, 0U);
    RecordProperty("max_multi_occultor_position_error_m", max_position);
    RecordProperty("max_multi_occultor_velocity_error_mps", max_velocity);
    const CartesianState initial{
        Epoch{0}, OriginId::earth, FrameId::gcrf, {7e6, 0, 0}, {0, 7500, 0}};
    options.srp.additional_eclipsing_bodies.push_back({ThirdBody::mars, 3396190});
    EXPECT_THROW((void)make_hpop(initial, options, environment), EphemerisCoverageError);
    options.srp.shadow_model = SrpShadowModel::none;
    const auto calls = planets->positions, checked = planets->certified;
    EXPECT_NO_THROW((void)make_hpop(initial, options, environment)->state_at(Epoch{1000000000}));
    EXPECT_EQ(planets->positions, calls);
    EXPECT_EQ(planets->certified, checked);
}
