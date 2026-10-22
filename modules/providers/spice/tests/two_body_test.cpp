#include <cmath>
#include <future>
#include <gtest/gtest.h>
#include <limits>
#include <numbers>
#include <orbelle/providers/spice/provider.hpp>

namespace {
using namespace orbelle;
using namespace orbelle::core;
constexpr double earth_mu = 3.986004418e14;

CartesianState state(std::array<double, 3> r, std::array<double, 3> v, Epoch epoch = Epoch{0}) {
    return {epoch, OriginId::earth, FrameId::gcrf, r, v};
}
void near(const CartesianState &actual, const CartesianState &expected, double position = 1e-8,
          double velocity = 1e-8) {
    EXPECT_EQ(actual.epoch, expected.epoch);
    EXPECT_EQ(actual.origin, expected.origin);
    EXPECT_EQ(actual.frame, expected.frame);
    for (std::size_t k = 0; k < 3; ++k) {
        EXPECT_NEAR(actual.position_m[k], expected.position_m[k], position);
        EXPECT_NEAR(actual.velocity_mps[k], expected.velocity_mps[k], velocity);
    }
}
std::array<double, 3> cross(std::array<double, 3> a, std::array<double, 3> b) {
    return {a[1] * b[2] - a[2] * b[1], a[2] * b[0] - a[0] * b[2], a[0] * b[1] - a[1] * b[0]};
}
double norm(const std::array<double, 3> &v) { return std::hypot(v[0], v[1], v[2]); }
double energy(const CartesianState &s) {
    const auto v = norm(s.velocity_mps);
    return v * v / 2 - earth_mu / norm(s.position_m);
}
} // namespace

TEST(TwoBody, InitialEpochPreservesAllBitsAndMetadataWithoutKernelFiles) {
    auto initial = state({7e6, -100.125, 200.75}, {1.25, 5000.125, -5300.25}, Epoch{123456789});
    const auto provider =
        providers::spice::make_two_body(initial, GravitationalParameter{earth_mu});
    const auto actual = provider->state_at(initial.epoch);
    EXPECT_EQ(actual.position_m, initial.position_m);
    EXPECT_EQ(actual.velocity_mps, initial.velocity_mps);
    EXPECT_EQ(actual.epoch, initial.epoch);
    EXPECT_EQ(actual.frame, FrameId::gcrf);
    // Provider owns the initial state, not a caller's mutable object.
    initial.position_m[0] = 8e6;
    EXPECT_DOUBLE_EQ(provider->state_at(initial.epoch).position_m[0], 7e6);
}

TEST(Keplerian, MatchesIndependentPerifocalGeometryForEllipsesAndHyperbolas) {
    for (const auto &[a, e] : {std::pair{9e6, 0.2}, std::pair{-9e6, 1.4}, std::pair{7e6, 0.0}})
        for (const double nu : {-1.2, 0.0, 1.2}) {
            const double p = a * (1 - e * e), r = p / (1 + e * std::cos(nu)),
                         speed = std::sqrt(earth_mu / p);
            // i=90°, Ω=90°, ω=0: perifocal X maps to GCRF Y and Y to Z.
            const KeplerianElements elements{Epoch{123456789},     a, e, std::numbers::pi / 2,
                                             std::numbers::pi / 2, 0, nu};
            near(providers::spice::to_cartesian(elements, GravitationalParameter{earth_mu}),
                 state({0, r * std::cos(nu), r * std::sin(nu)},
                       {0, -speed * std::sin(nu), speed * (e + std::cos(nu))}, elements.epoch),
                 1e-6, 1e-8);
        }
}
TEST(Keplerian, RoundTripsGeneralCircularEquatorialAndRetrogradeStates) {
    for (const double inclination : {0.0, 1e-6, 0.9, std::numbers::pi})
        for (const double eccentricity : {0.0, 0.1, 1.4}) {
            const KeplerianElements input{Epoch{8'000'000'000'000'000'000},
                                          eccentricity > 1 ? -9e6 : 9e6,
                                          eccentricity,
                                          inclination,
                                          0.8,
                                          0.6,
                                          -0.5};
            const auto cartesian =
                providers::spice::to_cartesian(input, GravitationalParameter{earth_mu});
            const auto recovered =
                providers::spice::to_keplerian(cartesian, GravitationalParameter{earth_mu});
            near(providers::spice::to_cartesian(recovered, GravitationalParameter{earth_mu}),
                 cartesian, 0.001, 1e-6);
        }
}
TEST(Keplerian, NearParabolicTrueAnomalyAvoidsMeanAnomalyCancellation) {
    for (const double e : {1 - 1e-8, 1 - 1e-10, 1 + 1e-10, 1 + 1e-8})
        for (const double nu : {-1.2, 1.2}) {
            const double q = 7e6, p = q * (1 + e), radius = p / (1 + e * std::cos(nu)),
                         speed = std::sqrt(earth_mu / p);
            const KeplerianElements elements{Epoch{0}, q / (1 - e), e, 0, 0, 0, nu};
            // Independent polar conic geometry, without eccentric/mean anomaly.
            near(providers::spice::to_cartesian(elements, GravitationalParameter{earth_mu}),
                 state({radius * std::cos(nu), radius * std::sin(nu), 0},
                       {-speed * std::sin(nu), speed * (e + std::cos(nu)), 0}),
                 1e-6, 1e-8);
        }
}
TEST(Keplerian, RejectsInvalidBranchesAndPreservesSpiceAfterFailure) {
    const auto workspace =
        std::filesystem::path(ORBELLE_SPICE_TEST_DIR) / "element-error-recovery";
    auto ephemeris = providers::spice::load_snapshot(ORBELLE_SPICE_SAMPLE, workspace);
    const auto epoch = ephemeris->data_info().first_epoch;
    const auto moon = ephemeris->geometric_state(301, 399, epoch);
    KeplerianElements input{Epoch{0}, 7e6, 0.1, 0.9, 0, 0, 0};
    auto invalid = input;
    invalid.eccentricity = 1;
    EXPECT_THROW((void)providers::spice::to_cartesian(invalid, GravitationalParameter{earth_mu}),
                 std::invalid_argument);
    invalid = input;
    invalid.inclination_rad = -1;
    EXPECT_THROW((void)providers::spice::to_cartesian(invalid, GravitationalParameter{earth_mu}),
                 std::invalid_argument);
    invalid = input;
    invalid.semi_major_axis_m = -7e6;
    invalid.eccentricity = 1.4;
    invalid.true_anomaly_rad = std::numbers::pi;
    EXPECT_THROW((void)providers::spice::to_cartesian(invalid, GravitationalParameter{earth_mu}),
                 std::invalid_argument);
    auto parabolic = state({3, 0, 0}, {0, 2, 0});
    EXPECT_THROW((void)providers::spice::to_keplerian(parabolic, GravitationalParameter{6}),
                 std::invalid_argument);
    // Actual library error, not wrapper validation: periapsis underflows to 0 km.
    invalid = input;
    invalid.semi_major_axis_m = std::numeric_limits<double>::denorm_min();
    invalid.eccentricity = 0;
    EXPECT_THROW((void)providers::spice::to_cartesian(invalid, GravitationalParameter{earth_mu}),
                 PropagationError);
    EXPECT_EQ(providers::spice::to_cartesian(input, GravitationalParameter{earth_mu}).epoch,
              Epoch{0});
    const auto recovered_moon = ephemeris->geometric_state(301, 399, epoch);
    EXPECT_EQ(recovered_moon.position_m, moon.position_m);
    EXPECT_EQ(recovered_moon.velocity_mps, moon.velocity_mps);
}

TEST(TwoBody, InclinedCircularGeometryUsesElapsedTaiSecondsInBothDirections) {
    // r=10, mu=1000: angular rate exactly 1 rad/s in the YZ plane.
    const auto initial = state({0, 10, 0}, {0, 0, 10}, Epoch{8'000'000'000'000'000'000});
    const auto provider = providers::spice::make_two_body(initial, GravitationalParameter{1000});
    for (const auto ns : {-1'234'567'891LL, 1LL, 1'234'567'891LL}) {
        const Epoch epoch{initial.epoch.tai_nanoseconds() + ns};
        const double angle = static_cast<double>(ns) * 1e-9;
        near(provider->state_at(epoch),
             state({0, 10 * std::cos(angle), 10 * std::sin(angle)},
                   {0, -10 * std::sin(angle), 10 * std::cos(angle)}, epoch));
    }
}

TEST(TwoBody, EllipticApocentreMatchesAnalyticalGeometry) {
    const auto provider = providers::spice::make_two_body(
        state({5, 0, 0}, {0, std::sqrt(300.0), 0}), GravitationalParameter{1000});
    const Epoch epoch{3'141'592'654};
    near(provider->state_at(epoch), state({-15, 0, 0}, {0, -std::sqrt(1000.0 / 30), 0}, epoch));
}

TEST(TwoBody, ParabolicBarkerSolutionIsFiniteAndReversible) {
    const auto provider =
        providers::spice::make_two_body(state({3, 0, 0}, {0, 2, 0}), GravitationalParameter{6});
    for (const auto direction : {-1, 1}) {
        const Epoch epoch{direction * 4'000'000'000LL};
        near(provider->state_at(epoch),
             state({0, direction * 6.0, 0}, {-direction * 1.0, 1, 0}, epoch));
    }
}

TEST(TwoBody, HyperbolicAnomalyMatchesIndependentAnalyticalPoint) {
    const auto provider =
        providers::spice::make_two_body(state({1, 0, 0}, {0, 2, 0}), GravitationalParameter{1});
    const double dt = std::sqrt(0.125) * (2.25 - std::log(2.0));
    const Epoch epoch{static_cast<std::int64_t>(std::llround(dt * 1e9))};
    near(provider->state_at(epoch), state({0.875, 0.375 * std::sqrt(8.0), 0},
                                          {-std::sqrt(2.0) * 0.75 / 2.75, 20.0 / 11.0, 0}, epoch));
}

TEST(TwoBody, EarthOrbitsConserveInvariantsAndRoundTrip) {
    const std::array initial_states{
        state({7e6, 0, 0}, {0, 5500, 5300}),
        state({42'164e3, 0, 0}, {0, -std::sqrt(earth_mu / 42'164e3), 0}),
        state({7e6, 0, 0}, {0, 8000, 9000}),
        state({7e6, 0, 0}, {0, std::sqrt(2 * earth_mu / 7e6) * (1 - 1e-8), 0}),
        state({7e6, 0, 0}, {0, std::sqrt(2 * earth_mu / 7e6) * (1 + 1e-8), 0})};
    for (const auto &initial : initial_states) {
        const auto provider =
            providers::spice::make_two_body(initial, GravitationalParameter{earth_mu});
        const auto h0 = cross(initial.position_m, initial.velocity_mps);
        const auto scale = std::max(earth_mu / norm(initial.position_m),
                                    std::pow(norm(initial.velocity_mps), 2) / 2);
        for (const auto seconds : {-86400LL, -1000LL, 1000LL, 86400LL}) {
            const auto actual = provider->state_at(Epoch{seconds * 1'000'000'000});
            EXPECT_LE(std::abs(energy(actual) - energy(initial)) / scale, 5e-12);
            auto dh = cross(actual.position_m, actual.velocity_mps);
            for (std::size_t k = 0; k < 3; ++k)
                dh[k] -= h0[k];
            EXPECT_LE(norm(dh) / norm(h0), 5e-12);
            const auto reverse =
                providers::spice::make_two_body(actual, GravitationalParameter{earth_mu});
            near(reverse->state_at(initial.epoch), initial, 0.005, 5e-6);
        }
    }
}

TEST(TwoBody, InvalidInitialConditionsAndFramesAreRejected) {
    const auto initial = state({7e6, 0, 0}, {0, 7500, 0});
    for (const auto mu : {0.0, -1.0, std::numeric_limits<double>::infinity(),
                          std::numeric_limits<double>::quiet_NaN()})
        EXPECT_THROW((void)providers::spice::make_two_body(initial, GravitationalParameter{mu}),
                     std::invalid_argument);
    for (const auto bad : {state({0, 0, 0}, {1, 0, 0}), state({7e6, 0, 0}, {0, 0, 0}),
                           state({7e6, 0, 0}, {100, 0, 0}),
                           state({std::numeric_limits<double>::quiet_NaN(), 0, 0}, {0, 7500, 0})})
        EXPECT_THROW((void)providers::spice::make_two_body(bad, GravitationalParameter{earth_mu}),
                     std::invalid_argument);
    for (const auto frame : {FrameId::itrf, FrameId::spice_j2000}) {
        auto bad = initial;
        bad.frame = frame;
        EXPECT_THROW((void)providers::spice::make_two_body(bad, GravitationalParameter{earth_mu}),
                     std::invalid_argument);
    }
}

TEST(TwoBody, NumericalFailureDoesNotPoisonSharedSpiceEngine) {
    EXPECT_THROW((void)providers::spice::make_two_body(state({1e-150, 0, 0}, {0, 1e-150, 0}),
                                                       GravitationalParameter{1}),
                 PropagationError);
    const auto good =
        providers::spice::make_two_body(state({3, 0, 0}, {0, 2, 0}), GravitationalParameter{6});
    near(good->state_at(Epoch{4'000'000'000}), state({0, 6, 0}, {-1, 1, 0}, Epoch{4'000'000'000}));
}

TEST(TwoBody, ConcurrentPropagationAndEphemeridesMatchSerialQueries) {
    const auto workspace =
        std::filesystem::path(ORBELLE_SPICE_TEST_DIR) / "two-body-concurrency";
    auto ephemeris = providers::spice::load_snapshot(ORBELLE_SPICE_SAMPLE, workspace);
    const auto time = ephemeris->data_info().first_epoch;
    const auto moon = ephemeris->geometric_state(301, 399, time);
    const auto a = providers::spice::make_two_body(state({7e6, 0, 0}, {0, 5500, 5300}, time),
                                                   GravitationalParameter{earth_mu});
    const auto b = providers::spice::make_two_body(state({7e6, 0, 0}, {0, 8000, 9000}, time),
                                                   GravitationalParameter{earth_mu});
    const Epoch target{time.tai_nanoseconds() + 1000'000'000'000};
    const auto expected_a = a->state_at(target), expected_b = b->state_at(target);
    std::vector<std::future<void>> workers;
    for (int i = 0; i < 4; ++i)
        workers.push_back(std::async(std::launch::async, [&, i] {
            for (int j = 0; j < 20; ++j) {
                const auto actual = (i % 2 ? a : b)->state_at(target);
                const auto &expected = i % 2 ? expected_a : expected_b;
                EXPECT_EQ(actual.position_m, expected.position_m);
                EXPECT_EQ(actual.velocity_mps, expected.velocity_mps);
                const auto current_moon = ephemeris->geometric_state(301, 399, time);
                EXPECT_EQ(current_moon.position_m, moon.position_m);
                EXPECT_EQ(current_moon.velocity_mps, moon.velocity_mps);
            }
        }));
    for (auto &worker : workers)
        worker.get();
}
