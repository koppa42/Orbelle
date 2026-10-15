#include <cmath>
#include <fstream>
#include <future>
#include <gtest/gtest.h>
#include <iomanip>
#include <limits>
#include <numbers>
#include <orbitforge/core/propagation/hpop.hpp>
#include <sstream>

using namespace orbitforge::core;
namespace {
CartesianState circular() {
    return {
        Epoch{0}, OriginId::earth, FrameId::gcrf, {7e6, 0, 0}, {0, std::sqrt(egm96_mu / 7e6), 0}};
}
HpopOptions point_mass() {
    HpopOptions o;
    o.gravity_degree = o.gravity_order = 0;
    return o;
}
class FixedFrames final : public IFrameProvider {
  public:
    PositionRotation gcrf_to_itrf(Epoch epoch, const EarthOrientationSample &) const override {
        return {epoch,
                OriginId::earth,
                FrameId::gcrf,
                FrameId::itrf,
                {{{1, 0, 0}, {0, 1, 0}, {0, 0, 1}}},
                "test-identity",
                "test"};
    }
    StateTransform gcrf_to_itrf_state(Epoch epoch,
                                      const EarthOrientationState &eop) const override {
        return {gcrf_to_itrf(epoch, eop.sample), {}, "test-static"};
    }
};
class FixedEop final : public IEarthOrientationProvider {
    EopDataInfo info_{"test",
                      "",
                      "",
                      "",
                      "",
                      Epoch{-1000000000000000000},
                      Epoch{1000000000000000000},
                      {"", "", "", 0, 100000}};

  public:
    EarthOrientationState at(Epoch epoch) const override {
        return {{epoch, -32, 0, 0, 0, 0, "test"}, {0, 0, 0, 0, 0}};
    }
    const EopDataInfo &data_info() const noexcept override { return info_; }
};
double position_error(const CartesianState &a, const CartesianState &b) {
    return std::hypot(a.position_m[0] - b.position_m[0], a.position_m[1] - b.position_m[1],
                      a.position_m[2] - b.position_m[2]);
}
} // namespace
TEST(Hpop, SatelliteMassValidatesCompatibleAreasEvenForDisabledForceDrafts) {
    auto options = point_mass();
    EXPECT_EQ(options.satellite_mass_kg, 1000);
    EXPECT_EQ(options.drag_area_m2(), 20);
    options.satellite_mass_kg = 1250;
    options.srp.area_mass_ratio_m2_per_kg = .03;
    EXPECT_EQ(options.drag_area_m2(), 25);
    EXPECT_EQ(options.srp_area_m2(), 37.5);
    for (double mass :
         {0., -1., std::numeric_limits<double>::infinity(),
          std::numeric_limits<double>::quiet_NaN(), std::numeric_limits<double>::denorm_min()}) {
        options.satellite_mass_kg = mass;
        EXPECT_THROW(validate_hpop_options(options), std::invalid_argument);
        EXPECT_THROW((void)make_hpop(circular(), options), std::invalid_argument);
    }
    options.satellite_mass_kg = std::numeric_limits<double>::max();
    options.srp.area_mass_ratio_m2_per_kg = 2;
    EXPECT_THROW(validate_hpop_options(options), std::invalid_argument);
    options.srp.area_mass_ratio_m2_per_kg = 0;
    EXPECT_NO_THROW(validate_hpop_options(options));
    EXPECT_EQ(options.srp_area_m2(), 0);
    options.drag.area_mass_ratio_m2_per_kg = 0;
    options.satellite_mass_kg = std::numeric_limits<double>::denorm_min();
    EXPECT_NO_THROW(validate_hpop_options(options));
    EXPECT_EQ(options.drag_area_m2(), 0);
}
TEST(Hpop, PointMassMatchesCircularAnalyticSolutionForwardBackwardAndNearEpochLimits) {
    const double rate = std::sqrt(egm96_mu / std::pow(7e6, 3));
    for (auto integrator :
         {HpopIntegrator::rkf78, HpopIntegrator::rkv89, HpopIntegrator::bulirsch_stoer})
        for (auto origin : {0LL, 8'000'000'000'000'000'000LL}) {
            auto initial = circular();
            initial.epoch = Epoch{origin};
            auto options = point_mass();
            options.integrator = integrator;
            const auto provider = make_hpop(initial, options);
            for (auto seconds : {86400LL, -86400LL, 1701LL, 0LL, -5000LL}) {
                const auto state = provider->state_at(Epoch{origin + seconds * 1000000000LL});
                const double angle = rate * static_cast<double>(seconds);
                EXPECT_NEAR(state.position_m[0], 7e6 * std::cos(angle), 0.06);
                EXPECT_NEAR(state.position_m[1], 7e6 * std::sin(angle), 0.06);
                EXPECT_NEAR(state.velocity_mps[0], -7e6 * rate * std::sin(angle), 6e-5);
                EXPECT_NEAR(state.velocity_mps[1], 7e6 * rate * std::cos(angle), 6e-5);
            }
            EXPECT_EQ(provider->state_at(initial.epoch).position_m, initial.position_m);
            const auto tiny = provider->state_at(Epoch{origin + 1});
            EXPECT_NEAR(tiny.position_m[1], initial.velocity_mps[1] * 1e-9, 1e-16);
        }
}
TEST(Hpop, Rk4ConvergesAtFourthOrderAndSamplingDoesNotChangeTheSolution) {
    const auto initial = circular();
    auto options = point_mass();
    const auto reference = make_hpop(initial, options)->state_at(Epoch{6000000000000});
    options.integrator = HpopIntegrator::rk4;
    options.fixed_step_seconds = 20;
    const auto coarse = make_hpop(initial, options)->state_at(reference.epoch);
    options.fixed_step_seconds = 10;
    const auto provider = make_hpop(initial, options);
    const auto fine = provider->state_at(reference.epoch);
    EXPECT_GT(position_error(coarse, reference) / position_error(fine, reference), 14);
    EXPECT_LT(position_error(fine, reference), 0.1);
    const auto fresh = make_hpop(initial, options);
    for (auto ns : {777000000000LL, -199000000000LL, 1000000000LL})
        (void)fresh->state_at(Epoch{ns});
    EXPECT_EQ(fresh->state_at(reference.epoch).position_m, fine.position_m);
    auto job =
        std::async(std::launch::async, [&] { return provider->state_at(Epoch{-8123456789012}); });
    EXPECT_EQ(provider->state_at(reference.epoch).position_m, fine.position_m);
    EXPECT_EQ(job.get().position_m, fresh->state_at(Epoch{-8123456789012}).position_m);
}
TEST(Hpop, CancellationDoesNotPublishIncompleteCheckpointsOrPoisonLaterQueries) {
    auto provider = make_hpop(circular(), point_mass());
    int calls = 0;
    const auto cancel = [&] {
        if (++calls == 20)
            throw std::runtime_error("cancel");
    };
    EXPECT_THROW((void)provider->state_at(Epoch{9000000000000}, cancel), std::runtime_error);
    EXPECT_EQ(calls, 20);
    EXPECT_EQ(provider->state_at(Epoch{9000000000000}).position_m,
              make_hpop(circular(), point_mass())->state_at(Epoch{9000000000000}).position_m);
}
TEST(Hpop, ValidatesOptionsResourcesFramesAltitudeToleranceAndWorkBounds) {
    auto initial = circular();
    auto options = point_mass();
    EXPECT_THROW((void)make_hpop(initial), std::invalid_argument);
    initial.frame = FrameId::itrf;
    EXPECT_THROW((void)make_hpop(initial, options), std::invalid_argument);
    initial = circular();
    initial.position_m[0] = egm96_radius_m;
    EXPECT_THROW((void)make_hpop(initial, options), PropagationError);
    initial = circular();
    initial.velocity_mps[0] = std::numeric_limits<double>::infinity();
    EXPECT_THROW((void)make_hpop(initial, options), PropagationError);
    for (int degree : {-1, 1, 21}) {
        options.gravity_degree = degree;
        EXPECT_THROW(validate_hpop_options(options), std::invalid_argument);
    }
    options = point_mass();
    options.min_step_seconds = options.max_step_seconds = 1800;
    options.position_tolerance_m = 1e-10;
    options.velocity_tolerance_mps = 1e-13;
    options.relative_tolerance = 1e-14;
    initial = circular();
    EXPECT_THROW((void)make_hpop(initial, options)->state_at(Epoch{300000000000}),
                 PropagationError);
    EXPECT_THROW((void)make_hpop(circular(), point_mass())->state_at(Epoch{31622401000000000}),
                 PropagationError);
    options = point_mass();
    options.integrator = HpopIntegrator::rk4;
    options.fixed_step_seconds = 1e-6;
    EXPECT_THROW((void)make_hpop(circular(), options)->state_at(Epoch{1000000000}),
                 PropagationError);
}
TEST(EarthGravity, DegreeTwoZonalMatchesClosedFormIncludingThePoles) {
    const double j2 = std::sqrt(5.0) * 0.484165371736e-3;
    for (const std::array<double, 3> r :
         {std::array{7e6, 0., 0.}, {0., 0., 7e6}, {4e6, 5e6, 3e6}}) {
        const double radius = std::hypot(r[0], r[1], r[2]), z2 = r[2] * r[2] / (radius * radius);
        const double k =
            1.5 * j2 * egm96_mu * egm96_radius_m * egm96_radius_m / std::pow(radius, 5);
        const auto actual = egm96_acceleration(r, 2, 0);
        for (std::size_t i = 0; i < 3; ++i)
            EXPECT_NEAR(actual[i],
                        -egm96_mu * r[i] / std::pow(radius, 3) +
                            k * r[i] * (5 * z2 - (i == 2 ? 3 : 1)),
                        5e-14);
    }
}
TEST(Hpop, MatchesIndependentOrekitHolmesFeatherstoneAndDp853Reference) {
    std::ifstream file{ORBITFORGE_HPOP_REFERENCE};
    ASSERT_TRUE(file.good());
    const HpopEnvironment environment{std::make_shared<FixedFrames>(),
                                      std::make_shared<FixedEop>()};
    std::string line;
    int count = 0;
    while (std::getline(file, line)) {
        if (line.empty() || line[0] == '#')
            continue;
        std::istringstream row{line};
        HpopOptions options;
        double elapsed;
        auto initial = circular();
        std::array<double, 3> expected_r{}, expected_v{}, expected_a{};
        row >> options.gravity_degree >> options.gravity_order >> elapsed;
        for (auto &v : initial.position_m)
            row >> v;
        for (auto &v : initial.velocity_mps)
            row >> v;
        for (auto &v : expected_r)
            row >> v;
        for (auto &v : expected_v)
            row >> v;
        for (auto &v : expected_a)
            row >> v;
        ASSERT_FALSE(row.fail()) << line;
        SCOPED_TRACE(line);
        const auto acceleration =
            egm96_acceleration(initial.position_m, options.gravity_degree, options.gravity_order);
        for (auto integrator :
             {HpopIntegrator::rkf78, HpopIntegrator::rkv89, HpopIntegrator::bulirsch_stoer}) {
            options.integrator = integrator;
            const auto actual = make_hpop(initial, options, environment)
                                    ->state_at(Epoch{static_cast<std::int64_t>(elapsed * 1e9)});
            for (std::size_t i = 0; i < 3; ++i) {
                EXPECT_NEAR(acceleration[i], expected_a[i], 5e-13);
                EXPECT_NEAR(actual.position_m[i], expected_r[i], 0.15);
                EXPECT_NEAR(actual.velocity_mps[i], expected_v[i], 0.00015);
            }
        }
        ++count;
    }
    EXPECT_EQ(count, 60);
}

namespace {
class MovingMoon final : public IThirdBodyEphemeris {
    ThirdBodyDataInfo info_{"synthetic-moving-Moon",
                            "",
                            "analytic-v1",
                            Epoch{-86400'000'000'000},
                            Epoch{86400'000'000'000},
                            1.32712440018e20,
                            4.902800066e12};

  public:
    std::array<double, 3> position(ThirdBody body, Epoch at) const override {
        if (body != ThirdBody::moon || at.tai_nanoseconds() < info_.first.tai_nanoseconds() ||
            at.tai_nanoseconds() > info_.last.tai_nanoseconds())
            throw std::out_of_range("Moon stage outside certified interval");
        const double angle = static_cast<double>(at.tai_nanoseconds()) * 1e-9 * 2 *
                                 std::numbers::pi / (27.321661 * 86400) +
                             .4;
        return {384400000 * std::cos(angle), 384400000 * std::sin(angle) * std::cos(.3),
                384400000 * std::sin(angle) * std::sin(.3)};
    }
    void validate_interval(Epoch first, Epoch last, std::span<const ThirdBody>) const override {
        if (first.tai_nanoseconds() < info_.first.tai_nanoseconds() ||
            last.tai_nanoseconds() > info_.last.tai_nanoseconds() ||
            first.tai_nanoseconds() > last.tai_nanoseconds())
            throw std::out_of_range("Moon coverage");
    }
    const ThirdBodyDataInfo &data_info() const noexcept override { return info_; }
};
CartesianState eccentric_analytic(double seconds) {
    constexpr double e = .7, perigee = 7e6;
    const double a = perigee / (1 - e), rate = std::sqrt(egm96_mu / (a * a * a));
    const double mean = rate * seconds;
    double anomaly = mean;
    for (int i = 0; i < 30; ++i)
        anomaly -= (anomaly - e * std::sin(anomaly) - mean) / (1 - e * std::cos(anomaly));
    const double c = std::cos(anomaly), sn = std::sin(anomaly), beta = std::sqrt(1 - e * e);
    return {Epoch{static_cast<std::int64_t>(seconds * 1e9)},
            OriginId::earth,
            FrameId::gcrf,
            {a * (c - e), a * beta * sn, 0},
            {-a * rate * sn / (1 - e * c), a * rate * beta * c / (1 - e * c), 0}};
}
} // namespace
TEST(Hpop, Rkv89MatchesIndependentScipyDop853ForEccentricJ2AndTimeVaryingForces) {
    std::ifstream file{ORBITFORGE_RKV89_REFERENCE};
    ASSERT_TRUE(file.good());
    HpopEnvironment environment{std::make_shared<FixedFrames>(), std::make_shared<FixedEop>()};
    environment.ephemeris = std::make_shared<MovingMoon>();
    std::string line;
    int count = 0;
    double max_position = 0, max_velocity = 0;
    while (std::getline(file, line)) {
        if (line.empty() || line[0] == '#')
            continue;
        SCOPED_TRACE(line);
        std::istringstream row{line};
        int kind;
        double seconds;
        auto initial = circular(), expected = initial;
        row >> kind >> seconds;
        for (auto &v : initial.position_m)
            row >> v;
        for (auto &v : initial.velocity_mps)
            row >> v;
        for (auto &v : expected.position_m)
            row >> v;
        for (auto &v : expected.velocity_mps)
            row >> v;
        ASSERT_FALSE(row.fail());
        expected.epoch = Epoch{std::llround(seconds * 1e9)};
        auto options = point_mass();
        options.integrator = HpopIntegrator::rkv89;
        options.gravity_degree = kind ? 2 : 0;
        options.relative_tolerance = 1e-14;
        options.position_tolerance_m = 1e-8;
        options.velocity_tolerance_mps = 1e-11;
        options.max_step_seconds = 300;
        options.third_bodies[static_cast<std::size_t>(ThirdBody::moon)].enabled = kind == 2;
        const auto actual = make_hpop(initial, options, environment)->state_at(expected.epoch);
        const double er = position_error(actual, expected);
        const double ev = std::hypot(actual.velocity_mps[0] - expected.velocity_mps[0],
                                     actual.velocity_mps[1] - expected.velocity_mps[1],
                                     actual.velocity_mps[2] - expected.velocity_mps[2]);
        max_position = std::max(max_position, er);
        max_velocity = std::max(max_velocity, ev);
        EXPECT_LT(er, .005);
        EXPECT_LT(ev, 5e-6);
        ++count;
    }
    EXPECT_EQ(count, 60);
    std::ostringstream metrics_r, metrics_v;
    metrics_r << std::setprecision(17) << max_position;
    metrics_v << std::setprecision(17) << max_velocity;
    RecordProperty("max_position_error_m", metrics_r.str());
    RecordProperty("max_velocity_error_mps", metrics_v.str());
}
TEST(Hpop, BulirschStoerMatchesIndependentHipparchusForEccentricJ2AndTimeVaryingForces) {
    std::ifstream file{ORBITFORGE_BS_REFERENCE};
    ASSERT_TRUE(file.good());
    HpopEnvironment environment{std::make_shared<FixedFrames>(), std::make_shared<FixedEop>()};
    environment.ephemeris = std::make_shared<MovingMoon>();
    std::string line;
    int count = 0;
    double max_position = 0, max_velocity = 0;
    while (std::getline(file, line)) {
        if (line.empty() || line[0] == '#')
            continue;
        SCOPED_TRACE(line);
        std::istringstream row{line};
        int kind;
        double seconds;
        auto initial = circular(), expected = initial;
        row >> kind >> seconds;
        for (auto &v : initial.position_m)
            row >> v;
        for (auto &v : initial.velocity_mps)
            row >> v;
        for (auto &v : expected.position_m)
            row >> v;
        for (auto &v : expected.velocity_mps)
            row >> v;
        ASSERT_FALSE(row.fail());
        expected.epoch = Epoch{std::llround(seconds * 1e9)};
        auto options = point_mass();
        options.integrator = HpopIntegrator::bulirsch_stoer;
        options.gravity_degree = kind ? 2 : 0;
        options.relative_tolerance = 1e-14;
        options.position_tolerance_m = 1e-8;
        options.velocity_tolerance_mps = 1e-11;
        options.max_step_seconds = 300;
        options.third_bodies[static_cast<std::size_t>(ThirdBody::moon)].enabled = kind == 2;
        const auto actual = make_hpop(initial, options, environment)->state_at(expected.epoch);
        const double er = position_error(actual, expected);
        const double ev = std::hypot(actual.velocity_mps[0] - expected.velocity_mps[0],
                                     actual.velocity_mps[1] - expected.velocity_mps[1],
                                     actual.velocity_mps[2] - expected.velocity_mps[2]);
        max_position = std::max(max_position, er);
        max_velocity = std::max(max_velocity, ev);
        EXPECT_LT(er, .005);
        EXPECT_LT(ev, 5e-6);
        ++count;
    }
    EXPECT_EQ(count, 60);
    std::ostringstream metrics_r, metrics_v;
    metrics_r << std::setprecision(17) << max_position;
    metrics_v << std::setprecision(17) << max_velocity;
    RecordProperty("max_position_error_m", metrics_r.str());
    RecordProperty("max_velocity_error_mps", metrics_v.str());
}
TEST(Hpop, Rkv89AdvancesTheEighthOrderMemberAndControlsRejectedSteps) {
    const auto initial = eccentric_analytic(0), expected = eccentric_analytic(6000);
    auto options = point_mass();
    options.integrator = HpopIntegrator::rkv89;
    // Loose tolerances force equal steps for the order test, with no error rejection.
    options.relative_tolerance = 1e-3;
    options.position_tolerance_m = 1;
    options.velocity_tolerance_mps = .001;
    options.min_step_seconds = options.max_step_seconds = 300;
    const auto coarse = make_hpop(initial, options)->state_at(expected.epoch);
    options.min_step_seconds = options.max_step_seconds = 150;
    const auto fine = make_hpop(initial, options)->state_at(expected.epoch);
    const double ratio = position_error(coarse, expected) / position_error(fine, expected);
    EXPECT_GT(ratio, 220); // Eighth order predicts 2^8; ninth-order advancement is excluded.
    EXPECT_LT(ratio, 330);
    EXPECT_LT(position_error(fine, expected), .001);
    RecordProperty("step_halving_error_ratio", std::to_string(ratio));
    options.relative_tolerance = 1e-14;
    options.position_tolerance_m = 1e-10;
    options.velocity_tolerance_mps = 1e-13;
    options.min_step_seconds = options.max_step_seconds = 300;
    EXPECT_THROW((void)make_hpop(initial, options)->state_at(Epoch{300'000'000'000}),
                 PropagationError);
    options.min_step_seconds = .001;
    const auto adaptive = make_hpop(initial, options)->state_at(expected.epoch);
    EXPECT_LT(position_error(adaptive, expected), 1e-4);
}
TEST(Hpop, AdaptiveCancellationAndQueryOrderPreserveDeterministicCheckpoints) {
    for (auto integrator : {HpopIntegrator::rkv89, HpopIntegrator::bulirsch_stoer}) {
        auto options = point_mass();
        options.integrator = integrator;
        const auto provider = make_hpop(circular(), options),
                   fresh = make_hpop(circular(), options);
        int calls = 0;
        EXPECT_THROW((void)provider->state_at(Epoch{9000'000'000'000},
                                              [&] {
                                                  if (++calls == 20)
                                                      throw std::runtime_error("cancel");
                                              }),
                     std::runtime_error);
        EXPECT_EQ(calls, 20);
        for (auto ns : {777'000'000'000LL, -199'000'000'000LL, 1LL})
            (void)provider->state_at(Epoch{ns});
        auto job = std::async(std::launch::async,
                              [&] { return provider->state_at(Epoch{-8123'456'789'012}); });
        EXPECT_EQ(provider->state_at(Epoch{9000'000'000'000}).position_m,
                  fresh->state_at(Epoch{9000'000'000'000}).position_m);
        const auto actual = job.get(), expected = fresh->state_at(Epoch{-8123'456'789'012});
        EXPECT_EQ(actual.position_m, expected.position_m);
        EXPECT_EQ(actual.velocity_mps, expected.velocity_mps);
    }
}

TEST(Hpop, BulirschStoerAdaptsOrderAndRejectsUnresolvedMinimumSteps) {
    const auto initial = eccentric_analytic(0), expected = eccentric_analytic(6000);
    auto options = point_mass();
    options.integrator = HpopIntegrator::bulirsch_stoer;
    options.min_step_seconds = options.max_step_seconds = 300;
    options.relative_tolerance = 1e-6;
    options.position_tolerance_m = .1;
    options.velocity_tolerance_mps = 1e-4;
    int loose_work = 0, tight_work = 0;
    const auto loose = make_hpop(initial, options)->state_at(expected.epoch, [&] { ++loose_work; });
    options.relative_tolerance = 1e-14;
    options.position_tolerance_m = 1e-8;
    options.velocity_tolerance_mps = 1e-11;
    const auto tight = make_hpop(initial, options)->state_at(expected.epoch, [&] { ++tight_work; });
    EXPECT_LT(position_error(tight, expected), .001);
    EXPECT_LT(position_error(tight, expected), position_error(loose, expected) * .01);
    EXPECT_GT(tight_work, loose_work);
    options.min_step_seconds = options.max_step_seconds = 60;
    const auto small_steps = make_hpop(initial, options)->state_at(expected.epoch);
    EXPECT_LT(position_error(small_steps, tight), .001);
    // Fast hyperbolic departure needs a shorter step than the allowed 300 seconds.
    auto fast = initial;
    fast.velocity_mps[1] = 120000;
    options.min_step_seconds = options.max_step_seconds = 300;
    EXPECT_THROW((void)make_hpop(fast, options)->state_at(Epoch{300'000'000'000}),
                 PropagationError);
    options.min_step_seconds = .001;
    EXPECT_NO_THROW((void)make_hpop(fast, options)->state_at(Epoch{300'000'000'000}));
    RecordProperty("loose_work", loose_work);
    RecordProperty("tight_work", tight_work);
}
