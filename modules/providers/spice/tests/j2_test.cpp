#include <cmath>
#include <future>
#include <gtest/gtest.h>
#include <limits>
#include <numbers>
#include <orbelle/providers/spice/provider.hpp>
#include <orbelle/core/propagation/orbit_design.hpp>

namespace {
using namespace orbelle;
using namespace orbelle::core;
const auto &earth = central_body(OriginId::earth);
constexpr double rad = std::numbers::pi / 180;
TEST(OrbitDesignPropagation, CircularRepeatClosesInUniformlyRotatingEarthFrame) {
    for (auto type : {EarthOrbitType::repeating_ground_trace, EarthOrbitType::repeating_sun_sync}) {
        EarthOrbitDesignRequest request;
        request.type = type;
        request.repeat_revolutions = 233;
        request.repeat_days = 16;
        request.raan_rad = 0.3;
        request.true_anomaly_rad = 0.4;
        const auto design = design_earth_orbit(request);
        const auto &e = design.elements;
        // Circular: mean anomaly equals true anomaly. This exercises the actual
        // propagation provider, independently of the designer's rate diagnostics.
        const auto provider = providers::spice::make_j2(
            {e.epoch, e.semi_major_axis_m, 0, e.inclination_rad, e.raan_rad, 0, e.true_anomaly_rad}, earth, {});
        const auto first = provider->state_at(e.epoch);
        for (int cycles : {-1, 1, 10}) {
            const double seconds = cycles * design.repeat_cycle_seconds;
            const auto end = provider->state_at(Epoch{static_cast<std::int64_t>(std::llround(seconds * 1e9))});
            const double rotation = 7.292115e-5 * seconds;
            const std::array fixed{std::cos(rotation) * end.position_m[0] + std::sin(rotation) * end.position_m[1],
                                  -std::sin(rotation) * end.position_m[0] + std::cos(rotation) * end.position_m[1],
                                  end.position_m[2]};
            for (std::size_t k = 0; k < 3; ++k) EXPECT_NEAR(fixed[k], first.position_m[k], 0.001);
        }
    }
}
J2MeanElements leo() { return {Epoch{0}, 7e6, .01, 98.5 * rad, 25 * rad, 70 * rad, 10 * rad}; }
void near(const CartesianState &a, const CartesianState &b, double r = .001, double v = 1e-6) {
    EXPECT_EQ(a.epoch, b.epoch);
    EXPECT_EQ(a.frame, FrameId::gcrf);
    for (std::size_t k = 0; k < 3; ++k) {
        EXPECT_NEAR(a.position_m[k], b.position_m[k], r);
        EXPECT_NEAR(a.velocity_mps[k], b.velocity_mps[k], v);
    }
}
TEST(J2Secular, MatchesIndependentKeplerSolverAtPositiveAndNegativeEpochs) {
    // Independent eccentric-anomaly Newton solver + perifocal rotations, SI.
    // Reference values and generation method are documented in p1_j2_frames.md.
    const auto provider = providers::spice::make_j2(leo(), earth, {});
    const std::array times{-2592000LL, -86400LL, 0LL, 86400LL, 2592000LL};
    const std::array<std::array<double, 3>, 5> expected{
        {{6507636.81865, -1160843.78179, 2471843.39807},
         {-5390492.46583, -2937589.91476, 3331609.01856},
         {1495693.94831, -416444.777990, 6754954.55088},
         {6248041.05824, 2877011.06403, 1075696.94996},
         {-2332435.18905, -4837187.17656, 4594793.66893}}};
    for (std::size_t i = 0; i < times.size(); ++i) {
        const auto state = provider->state_at(Epoch{times[i] * 1'000'000'000});
        for (std::size_t k = 0; k < 3; ++k)
            EXPECT_NEAR(state.position_m[k], expected[i][k], .001);
    }
    const auto eccentric = providers::spice::make_j2(
        {Epoch{0}, 26600e3, .7, std::acos(std::sqrt(.2)), 15 * rad, 45 * rad, 120 * rad}, earth,
        {});
    const auto state = eccentric->state_at(Epoch{2592000'000'000'000LL});
    const std::array high_expected{-33344746.1568, -17326431.5603, -20623081.2581};
    for (std::size_t k = 0; k < 3; ++k)
        EXPECT_NEAR(state.position_m[k], high_expected[k], .001);
}
TEST(J2Secular, VelocityIsDerivativeOfThePrecessingPosition) {
    for (const double inclination : {0., .5, std::numbers::pi / 2, 2.4, std::numbers::pi}) {
        auto elements = leo();
        elements.inclination_rad = inclination;
        for (const J2SecularOptions options : {J2SecularOptions{},
                                               {true, false, false},
                                               {false, true, false},
                                               {false, false, true}}) {
            const auto provider = providers::spice::make_j2(elements, earth, options);
            for (const auto seconds : {-86400LL, 0LL, 86400LL}) {
                const auto t = seconds * 1'000'000'000;
                const auto state = provider->state_at(Epoch{t});
                const auto m2 = provider->state_at(Epoch{t - 250'000'000});
                const auto m1 = provider->state_at(Epoch{t - 125'000'000});
                const auto p1 = provider->state_at(Epoch{t + 125'000'000});
                const auto p2 = provider->state_at(Epoch{t + 250'000'000});
                for (std::size_t k = 0; k < 3; ++k) {
                    const auto derivative = (m2.position_m[k] - 8 * m1.position_m[k] +
                                             8 * p1.position_m[k] - p2.position_m[k]) /
                                            1.5;
                    EXPECT_NEAR(state.velocity_mps[k], derivative, 2e-5);
                }
            }
        }
    }
}
TEST(J2Secular, CircularEquatorialMotionHasAnalyticAngularRate) {
    constexpr double a = 42164e3;
    const auto provider = providers::spice::make_j2({Epoch{0}, a, 0, 0, 0, 0, 0}, earth, {});
    const auto n = std::sqrt(earth.mu.metres_cubed_per_second_squared / (a * a * a));
    const auto rate = n * (1 + 3 * earth.j2 * std::pow(earth.equatorial_radius_m / a, 2));
    const auto state = provider->state_at(Epoch{86400'000'000'000LL});
    EXPECT_NEAR(state.position_m[0], a * std::cos(rate * 86400), 1e-6);
    EXPECT_NEAR(state.position_m[1], a * std::sin(rate * 86400), 1e-6);
    EXPECT_NEAR(state.velocity_mps[0], -rate * state.position_m[1], 1e-9);
    EXPECT_NEAR(state.velocity_mps[1], rate * state.position_m[0], 1e-9);
}
TEST(J2Secular, ZeroJ2AndDisabledTermsRecoverTwoBody) {
    auto zero = earth;
    zero.j2 = 0;
    const auto initial = providers::spice::j2_mean_ellipse_state(leo(), earth);
    const auto reference = providers::spice::make_two_body(initial, earth.mu);
    for (const auto &provider : {providers::spice::make_j2(leo(), zero, {}),
                                 providers::spice::make_j2(leo(), earth, {false, false, false})})
        for (const auto seconds : {-86400LL, 0LL, 86400LL, 2592000LL})
            near(provider->state_at(Epoch{seconds * 1'000'000'000}),
                 reference->state_at(Epoch{seconds * 1'000'000'000}));
}
TEST(J2Secular, AuxiliaryEllipseRoundTripsWithoutClaimingOsculatingConversion) {
    for (const auto inclination : {0., .5, std::numbers::pi}) {
        for (const auto eccentricity : {0., .01, .7}) {
            auto elements = leo();
            elements.semi_major_axis_m = 26600e3;
            elements.inclination_rad = inclination;
            elements.eccentricity = eccentricity;
            const auto state = providers::spice::j2_mean_ellipse_state(elements, earth);
            const auto recovered = providers::spice::j2_mean_ellipse_elements(state, earth);
            near(providers::spice::j2_mean_ellipse_state(recovered, earth), state, 1e-6, 1e-9);
        }
    }
}
TEST(J2Secular, RejectsOpenSubsurfaceNonfiniteAndWrongFrameInputs) {
    for (const auto eccentricity : {-1., 1., 1.4, std::numeric_limits<double>::quiet_NaN()}) {
        auto bad = leo();
        bad.eccentricity = eccentricity;
        EXPECT_THROW((void)providers::spice::make_j2(bad, earth, {}), std::invalid_argument);
    }
    auto bad = leo();
    bad.semi_major_axis_m = earth.equatorial_radius_m;
    EXPECT_THROW((void)providers::spice::make_j2(bad, earth, {}), std::invalid_argument);
    auto state = providers::spice::j2_mean_ellipse_state(leo(), earth);
    state.frame = FrameId::itrf;
    EXPECT_THROW((void)providers::spice::j2_mean_ellipse_elements(state, earth),
                 std::invalid_argument);
    auto invalid_body = earth;
    invalid_body.mu = {-1};
    EXPECT_THROW((void)providers::spice::make_j2(leo(), invalid_body, {}), std::invalid_argument);
}
TEST(J2Secular, ImmutableProviderIsSafeForConcurrentRandomAccess) {
    const auto provider = providers::spice::make_j2(leo(), earth, {});
    std::vector<std::future<CartesianState>> futures;
    for (int k = 0; k < 16; ++k)
        futures.push_back(std::async(std::launch::async, [provider, k] {
            return provider->state_at(Epoch{static_cast<std::int64_t>(k - 8) * 3600'000'000'000LL});
        }));
    for (int k = 0; k < 16; ++k) {
        const auto state = futures[k].get();
        near(state, provider->state_at(state.epoch), 0, 0);
    }
}
} // namespace
