#include <chrono>
#include <cmath>
#include <fstream>
#include <future>
#include <gtest/gtest.h>
#include <iostream>
#include <limits>
#include <numbers>
#include <orbitforge/core/propagation/atmospheric_drag.hpp>
#include <orbitforge/core/propagation/hpop.hpp>
#include <orbitforge/core/propagation/state_provider.hpp>
#include <sstream>

using namespace orbitforge::core;
namespace {
constexpr double spin = 7.292115e-5;
class SyntheticTime final : public ITimeProvider {
    TimeDataInfo info_{"synthetic-utc-is-tai", std::string(64, 'a'), "", 0, 100000};

  public:
    Epoch from_utc(const UtcDateTime &u) const override {
        const auto date = std::chrono::sys_days{std::chrono::year{u.year} /
                                                std::chrono::month{static_cast<unsigned>(u.month)} /
                                                std::chrono::day{static_cast<unsigned>(u.day)}};
        const auto first = std::chrono::sys_days{std::chrono::year{2000} / 1 / 1};
        return Epoch{std::chrono::duration_cast<std::chrono::nanoseconds>(date - first).count() +
                     ((u.hour * 3600LL + u.minute * 60 + u.second) - 43200) * 1000000000 +
                     u.nanosecond};
    }
    UtcDateTime to_utc(Epoch e) const override {
        const auto total = std::chrono::nanoseconds{e.tai_nanoseconds() + 43200000000000};
        const auto days = std::chrono::floor<std::chrono::days>(total);
        const std::chrono::year_month_day date{
            std::chrono::sys_days{std::chrono::year{2000} / 1 / 1} + days};
        const std::chrono::hh_mm_ss hms{total - days};
        return {static_cast<int>(date.year()),
                static_cast<int>(static_cast<unsigned>(date.month())),
                static_cast<int>(static_cast<unsigned>(date.day())),
                static_cast<int>(hms.hours().count()),
                static_cast<int>(hms.minutes().count()),
                static_cast<int>(hms.seconds().count()),
                static_cast<int>(hms.subseconds().count())};
    }
    int tai_minus_utc_seconds(Epoch) const override { return 0; }
    JulianDate julian_date(Epoch e, TimeScale scale) const override {
        return {scale, 2451545, e.seconds_since(Epoch{0}) / 86400};
    }
    const TimeDataInfo &data_info() const noexcept override { return info_; }
};
class RotatingFrames final : public IFrameProvider {
  public:
    PositionRotation gcrf_to_itrf(Epoch epoch, const EarthOrientationSample &) const override {
        const double angle = spin * epoch.seconds_since(Epoch{0}), c = std::cos(angle),
                     s = std::sin(angle);
        return {epoch,
                OriginId::earth,
                FrameId::gcrf,
                FrameId::itrf,
                {{{c, s, 0}, {-s, c, 0}, {0, 0, 1}}},
                "synthetic-spin",
                "test"};
    }
    StateTransform gcrf_to_itrf_state(Epoch epoch,
                                      const EarthOrientationState &eop) const override {
        const double angle = spin * epoch.seconds_since(Epoch{0}), c = std::cos(angle),
                     s = std::sin(angle);
        return {gcrf_to_itrf(epoch, eop.sample),
                {{{-spin * s, spin * c, 0}, {-spin * c, -spin * s, 0}, {0, 0, 0}}},
                "analytic-spin-rate"};
    }
};
class SyntheticEop final : public IEarthOrientationProvider {
    EopDataInfo info_{"test",
                      std::string(64, 'b'),
                      "",
                      "",
                      "",
                      Epoch{-172800000000000},
                      Epoch{172800000000000},
                      SyntheticTime{}.data_info()};

  public:
    EarthOrientationState at(Epoch e) const override {
        return {{e, 0, 0, 0, 0, 0, "test"}, {0, 0, 0, 0, 0}};
    }
    const EopDataInfo &data_info() const noexcept override { return info_; }
};
class SyntheticSun final : public IThirdBodyEphemeris {
    ThirdBodyDataInfo info_{"test",
                            std::string(64, 'c'),
                            "test",
                            Epoch{-172800000000000},
                            Epoch{172800000000000},
                            1.327e20,
                            0};

  public:
    std::array<double, 3> position(ThirdBody body, Epoch e) const override {
        if (body != ThirdBody::sun)
            throw std::invalid_argument("Synthetic Sun only");
        const double phase =
            .03 + e.seconds_since(Epoch{0}) * 2 * std::numbers::pi / (365.25 * 86400);
        constexpr double au = 149597870700;
        return {au * std::cos(phase), au * std::sin(phase) * std::cos(.409),
                au * std::sin(phase) * std::sin(.409)};
    }
    void validate_interval(Epoch first, Epoch last,
                           std::span<const ThirdBody> bodies) const override {
        if (first.tai_nanoseconds() < info_.first.tai_nanoseconds() ||
            last.tai_nanoseconds() > info_.last.tai_nanoseconds())
            throw std::out_of_range("Synthetic Sun coverage");
        for (auto body : bodies)
            if (body != ThirdBody::sun)
                throw std::invalid_argument("Synthetic Sun only");
    }
    const ThirdBodyDataInfo &data_info() const noexcept override { return info_; }
};
HpopEnvironment environment() {
    return {std::make_shared<RotatingFrames>(),
            std::make_shared<SyntheticEop>(),
            {},
            std::make_shared<SyntheticSun>(),
            std::make_shared<SyntheticTime>()};
}
} // namespace
TEST(AtmosphericDrag, KpConvertsToApRatherThanUsingKpAsAp) {
    EXPECT_EQ(kp_to_ap(0), 0);
    EXPECT_EQ(kp_to_ap(3), 15);
    EXPECT_EQ(kp_to_ap(9), 400);
    EXPECT_NEAR(kp_to_ap(1.5), 5.5, 1e-14);
    EXPECT_THROW((void)kp_to_ap(-1), std::invalid_argument);
    EXPECT_THROW((void)kp_to_ap(10), std::invalid_argument);
}
TEST(AtmosphericDrag, InvalidDatesEnumsNonfiniteInputsAndModelDomainsAreRejected) {
    AtmosphereInput input;
    input.geodetic.height_m = 400000;
    input.utc = {2001, 2, 29, 12, 0, 0, 0};
    EXPECT_THROW((void)evaluate_atmosphere(AtmosphereModel::nrlmsise00, input),
                 std::invalid_argument);
    input.utc = {2000, 1, 1, 12, 0, 60, 0};
    EXPECT_THROW((void)evaluate_atmosphere(AtmosphereModel::jacchia_roberts, input),
                 std::invalid_argument);
    input.utc = {2000, 1, 1, 12, 0, 0, 0};
    EXPECT_THROW((void)evaluate_atmosphere(static_cast<AtmosphereModel>(99), input),
                 std::invalid_argument);
    input.f107 = std::numeric_limits<double>::quiet_NaN();
    EXPECT_THROW((void)evaluate_atmosphere(AtmosphereModel::nrlmsise00, input),
                 std::invalid_argument);
    input.f107 = 150;
    input.geodetic.height_m = 1000001;
    EXPECT_THROW((void)evaluate_atmosphere(AtmosphereModel::nrlmsise00, input), PropagationError);
    input.geodetic.height_m = 89999;
    EXPECT_THROW((void)evaluate_atmosphere(AtmosphereModel::jacchia_roberts, input),
                 PropagationError);
    input.geodetic.height_m = 2500001;
    EXPECT_THROW((void)evaluate_atmosphere(AtmosphereModel::jacchia_roberts, input),
                 PropagationError);
    EXPECT_THROW((void)drag_geodetic_position({0, 0, 0}), PropagationError);
    EXPECT_THROW((void)drag_geodetic_position({0, std::numeric_limits<double>::infinity(), 0}),
                 std::invalid_argument);
}
TEST(AtmosphericDrag, DensityMatchesIndependentAuthorNrlmsiseGtd7dIncludingLowAltitudeBoundaries) {
    std::ifstream source(ORBITFORGE_NRLMSISE_REFERENCE);
    ASSERT_TRUE(source);
    std::string line;
    std::size_t count = 0;
    double max_relative = 0;
    while (std::getline(source, line)) {
        if (line.empty() || line[0] == '#')
            continue;
        SCOPED_TRACE(line);
        std::istringstream row(line);
        AtmosphereInput input;
        double seconds, expected_density, temperature, exospheric;
        ASSERT_TRUE(row >> input.utc.year >> input.utc.month >> input.utc.day >> seconds >>
                    input.geodetic.height_m >> input.geodetic.latitude_rad >>
                    input.geodetic.longitude_rad >> input.solar_hour_angle_rad >>
                    input.sun_declination_rad >> input.f107 >> input.f107_average >> input.kp >>
                    expected_density >> temperature >> exospheric);
        if (seconds == 86400) {
            input.utc.hour = 23;
            input.utc.minute = 59;
            input.utc.second = 60;
        } else {
            input.utc.hour = static_cast<int>(seconds) / 3600;
            input.utc.minute = static_cast<int>(seconds) % 3600 / 60;
            input.utc.second = static_cast<int>(seconds) % 60;
        }
        const auto actual = evaluate_atmosphere(AtmosphereModel::nrlmsise00, input);
        const double relative = std::abs(actual.density_kg_m3 / expected_density - 1);
        max_relative = std::max(max_relative, relative);
        EXPECT_NEAR(actual.density_kg_m3, expected_density, expected_density * 2e-10);
        EXPECT_NEAR(actual.temperature_k, temperature, 1e-7);
        EXPECT_NEAR(actual.exospheric_temperature_k, exospheric, 1e-7);
        ++count;
    }
    EXPECT_EQ(count, 396U);
    std::cout << "NRLMSISE author reference: max relative density error " << max_relative << '\n';
}
TEST(AtmosphericDrag, JrNoonMidnightEquinoxAndPolarLimitsAreFiniteAndConcurrent) {
    AtmosphereInput input;
    input.geodetic = {.5, .6, 400000};
    const auto expected = evaluate_atmosphere(AtmosphereModel::jacchia_roberts, input);
    EXPECT_GT(expected.density_kg_m3, 0);
    for (double latitude : {-std::numbers::pi / 2, 0., std::numbers::pi / 2})
        for (double hour : {-std::numbers::pi, 0., std::numbers::pi}) {
            input.geodetic.latitude_rad = latitude;
            input.solar_hour_angle_rad = hour;
            EXPECT_GT(evaluate_atmosphere(AtmosphereModel::jacchia_roberts, input).density_kg_m3,
                      0);
        }
    input.geodetic.latitude_rad = .6;
    input.solar_hour_angle_rad = 0;
    const auto run = [input] {
        return evaluate_atmosphere(AtmosphereModel::jacchia_roberts, input).density_kg_m3;
    };
    auto a = std::async(std::launch::async, run), b = std::async(std::launch::async, run);
    EXPECT_EQ(a.get(), expected.density_kg_m3);
    EXPECT_EQ(b.get(), expected.density_kg_m3);
}
TEST(AtmosphericDrag, JrDensityMatchesUnmodifiedGmatScalarModelAcrossRegimesAndSeasonalDates) {
    std::ifstream source(ORBITFORGE_JR_REFERENCE);
    ASSERT_TRUE(source);
    std::string line;
    std::size_t count = 0;
    double maximum = 0;
    while (std::getline(source, line)) {
        if (line.empty() || line[0] == '#')
            continue;
        SCOPED_TRACE(line);
        std::istringstream row(line);
        AtmosphereInput input;
        double seconds, density;
        ASSERT_TRUE(row >> input.utc.year >> input.utc.month >> input.utc.day >> seconds >>
                    input.geodetic.height_m >> input.geodetic.latitude_rad >>
                    input.geodetic.longitude_rad >> input.solar_hour_angle_rad >>
                    input.sun_declination_rad >> input.f107 >> input.f107_average >> input.kp >>
                    density);
        if (seconds == 86400) {
            input.utc.hour = 23;
            input.utc.minute = 59;
            input.utc.second = 60;
        } else {
            input.utc.hour = static_cast<int>(seconds) / 3600;
            input.utc.minute = static_cast<int>(seconds) % 3600 / 60;
            input.utc.second = static_cast<int>(seconds) % 60;
        }
        const auto actual = evaluate_atmosphere(AtmosphereModel::jacchia_roberts, input);
        EXPECT_NEAR(actual.density_kg_m3, density, density * 2e-10);
        maximum = std::max(maximum, std::abs(actual.density_kg_m3 / density - 1));
        ++count;
    }
    EXPECT_EQ(count, 576U);
    std::cout << "JR GMAT reference: max relative density error " << maximum << '\n';
}
TEST(AtmosphericDrag, RelativeVelocityDirectionAreaAndLowAltitudeBlendActuallyAffectTheForce) {
    DragOptions options;
    options.enabled = true;
    const std::array<double, 3> r{6378137 + 400000, 0, 0}, v{0, 7200, 0}, sun{1e11, 0, 2e10};
    const auto original = evaluate_drag(r, v, sun, {2000, 1, 1, 12, 0, 0, 0}, options);
    EXPECT_GT(original.density_kg_m3, 0);
    EXPECT_EQ(original.acceleration_itrf_mps2[0], 0);
    EXPECT_LT(original.acceleration_itrf_mps2[1], 0);
    EXPECT_DOUBLE_EQ(original.acceleration_itrf_mps2[1],
                     -.5 * original.density_kg_m3 * 2.2 * .02 * 7200 * 7200);
    EXPECT_EQ(
        evaluate_drag(r, {0, 0, 0}, sun, {2000, 1, 1, 12, 0, 0, 0}, options).acceleration_itrf_mps2,
        (std::array{0., 0., 0.}));
    options.area_mass_ratio_m2_per_kg *= 2;
    EXPECT_EQ(
        evaluate_drag(r, v, sun, {2000, 1, 1, 12, 0, 0, 0}, options).acceleration_itrf_mps2[1],
        2 * original.acceleration_itrf_mps2[1]);
    options.low_altitude_model = LowAltitudeDensityModel::nrlmsise00;
    double previous = 0;
    for (double altitude : {80000., 89999., 90000., 90001., 119999., 120000., 120001.}) {
        const auto value =
            evaluate_drag({6378137 + altitude, 0, 0}, v, sun, {2000, 1, 1, 12, 0, 0, 0}, options);
        EXPECT_GT(value.density_kg_m3, 0);
        if (previous > 0 &&
            (altitude == 90000 || altitude == 90001 || altitude == 120000 || altitude == 120001)) {
            EXPECT_LT(std::abs(value.density_kg_m3 / previous - 1), .001);
        }
        previous = value.density_kg_m3;
    }
}
TEST(AtmosphericDrag, CoRotatingWgs84NrlmsisePropagationMatchesIndependentOrekit) {
    std::ifstream source(ORBITFORGE_DRAG_REFERENCE);
    ASSERT_TRUE(source);
    std::string line;
    std::size_t count = 0;
    double max_position = 0, max_velocity = 0;
    const auto env = environment();
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
        expected.epoch = Epoch{static_cast<std::int64_t>(seconds * 1e9)};
        HpopOptions options;
        options.gravity_degree = options.gravity_order = 0;
        options.max_step_seconds = 2.5;
        options.min_step_seconds = 1e-6;
        options.relative_tolerance = 1e-14;
        options.position_tolerance_m = 1e-8;
        options.velocity_tolerance_mps = 1e-11;
        options.drag.enabled = true;
        options.drag.atmosphere = AtmosphereModel::nrlmsise00;
        if (mode == 1) {
            options.drag.f107 = 70;
            options.drag.f107_average = 80;
            options.drag.kp = 0;
        }
        if (mode == 2) {
            options.drag.f107 = 250;
            options.drag.f107_average = 220;
            options.drag.kp = 8.5;
            options.drag.cd = 2.7;
            options.drag.area_mass_ratio_m2_per_kg = .035;
        }
        const auto transform =
            env.frames->gcrf_to_itrf_state(initial.epoch, env.eop->at(initial.epoch));
        const auto fixed = transform.apply(initial);
        const auto force = evaluate_drag(fixed.position_m, fixed.velocity_mps,
                                         transform.rotation.apply_position(env.ephemeris->position(
                                             ThirdBody::sun, initial.epoch)),
                                         env.time->to_utc(initial.epoch), options.drag);
        const auto inertial_force =
            transform.rotation.inverse_position(force.acceleration_itrf_mps2);
        const auto actual = make_hpop(initial, options, env)->state_at(expected.epoch);
        for (std::size_t i = 0; i < 3; ++i) {
            EXPECT_NEAR(inertial_force[i], acceleration[i], 1e-17);
            EXPECT_NEAR(actual.position_m[i], expected.position_m[i], .0005);
            EXPECT_NEAR(actual.velocity_mps[i], expected.velocity_mps[i], 5e-7);
            max_position =
                std::max(max_position, std::abs(actual.position_m[i] - expected.position_m[i]));
            max_velocity =
                std::max(max_velocity, std::abs(actual.velocity_mps[i] - expected.velocity_mps[i]));
        }
        ++count;
    }
    EXPECT_EQ(count, 24U);
    std::cout << "Drag Orekit reference: max position component " << max_position << " m; velocity "
              << max_velocity << " m/s\n";
}
TEST(AtmosphericDrag, RequiresUtcEopSunAndDeclaredApparentCoverageAndCancellationIsReusable) {
    const CartesianState initial{
        Epoch{0}, OriginId::earth, FrameId::gcrf, {6778137, 0, 0}, {0, 6300, 4300}};
    HpopOptions options;
    options.gravity_degree = options.gravity_order = 0;
    options.drag.enabled = true;
    auto env = environment();
    auto missing = env;
    missing.time.reset();
    EXPECT_THROW((void)make_hpop(initial, options, missing), std::invalid_argument);
    missing = env;
    missing.frames.reset();
    EXPECT_THROW((void)make_hpop(initial, options, missing), std::invalid_argument);
    missing = env;
    missing.eop.reset();
    EXPECT_THROW((void)make_hpop(initial, options, missing), std::invalid_argument);
    missing = env;
    missing.ephemeris.reset();
    EXPECT_THROW((void)make_hpop(initial, options, missing), std::invalid_argument);
    options.drag.use_apparent_sun_position = true;
    EXPECT_THROW((void)make_hpop(initial, options, env), std::invalid_argument);
    options.drag.use_apparent_sun_position = false;
    const auto provider = make_hpop(initial, options, env),
               fresh = make_hpop(initial, options, env);
    unsigned calls = 0;
    const Epoch target{6000000000000};
    EXPECT_THROW((void)provider->state_at(target,
                                          [&] {
                                              if (++calls == 40)
                                                  throw std::runtime_error("cancel-drag");
                                          }),
                 std::runtime_error);
    EXPECT_EQ(provider->state_at(target).position_m, fresh->state_at(target).position_m);
    EXPECT_THROW((void)provider->state_at(Epoch{172801000000000}), std::out_of_range);
}
