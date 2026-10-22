#include <chrono>
#include <cmath>
#include <gtest/gtest.h>
#include <limits>
#include <orbelle/core/propagation/hpop_covariance.hpp>

using namespace orbelle::core;
namespace {
HpopCovarianceInput identity_covariance() {
    HpopCovarianceInput input;
    for (std::size_t i = 0; i < 6; ++i)
        input.state[i][i] = 1;
    return input;
}
HpopVariationalState identity_transition() {
    HpopVariationalState result{
        {Epoch{0}, OriginId::earth, FrameId::gcrf, {7e6, 1e6, 2e5}, {-1000, 7200, 1200}}};
    for (std::size_t i = 0; i < 6; ++i)
        result.state_transition[i][i] = 1;
    return result;
}
} // namespace
TEST(HpopCovariance, RejectsInvalidActiveJointMatricesWithoutRepair) {
    auto input = identity_covariance();
    EXPECT_NO_THROW(validate_hpop_covariance(input));
    EXPECT_THROW(validate_hpop_covariance({}), std::invalid_argument);
    input.drag = HpopConsiderParameter{};
    EXPECT_THROW(validate_hpop_covariance(input), std::invalid_argument);
    input.drag->variance = -1;
    EXPECT_THROW(validate_hpop_covariance(input), std::invalid_argument);
    input.drag->variance = 1;
    input.drag->state_cross_covariance[0] = .8;
    input.srp = *input.drag;
    // Every pair is positive definite, but this complete joint matrix is not.
    EXPECT_THROW(validate_hpop_covariance(input), std::invalid_argument);
    input.drag_srp_covariance = .5;
    EXPECT_NO_THROW(validate_hpop_covariance(input));
    input.srp.reset();
    EXPECT_THROW(validate_hpop_covariance(input), std::invalid_argument);
    input = identity_covariance();
    input.state[0][1] = .01;
    EXPECT_THROW(validate_hpop_covariance(input), std::invalid_argument);
    input.state[1][0] = .01;
    EXPECT_NO_THROW(validate_hpop_covariance(input));
    input.state[1][0] = std::numeric_limits<double>::infinity();
    EXPECT_THROW(validate_hpop_covariance(input), std::invalid_argument);
    input = identity_covariance();
    input.state[0][1] = input.state[1][0] = 1;
    EXPECT_THROW(validate_hpop_covariance(input), std::invalid_argument);
    input.state[0][1] = input.state[1][0] = std::nextafter(1., 0.);
    EXPECT_NO_THROW(validate_hpop_covariance(input));
    input = identity_covariance();
    input.drag_srp_covariance = std::numeric_limits<double>::quiet_NaN();
    EXPECT_THROW(validate_hpop_covariance(input), std::invalid_argument);
}
TEST(HpopCovariance, DiagonalScalingRetainsTinyAndHugePhysicalUnits) {
    auto input = identity_covariance();
    const std::array<double, 6> diagonal{1e-280, 1e280, 1e-200, 1e200, 1e-100, 1e100};
    for (std::size_t i = 0; i < 6; ++i)
        input.state[i][i] = diagonal[i];
    input.state[0][1] = input.state[1][0] = .25;
    const auto original = input;
    EXPECT_NO_THROW(validate_hpop_covariance(input));
    const auto actual = transport_hpop_covariance(input, identity_transition());
    for (std::size_t i = 0; i < 6; ++i)
        for (std::size_t j = 0; j < 6; ++j) {
            const double a = std::sqrt(diagonal[i]), b = std::sqrt(diagonal[j]);
            const double difference = (actual.covariance.state[i][j] - input.state[i][j]) /
                                      std::max(a, b) / std::min(a, b);
            EXPECT_LE(std::abs(difference), 2e-15);
            EXPECT_EQ(actual.covariance.state[i][j], actual.covariance.state[j][i]);
        }
    EXPECT_EQ(original, input);
    auto small_asymmetry = identity_covariance();
    small_asymmetry.state[0][1] = .25;
    small_asymmetry.state[1][0] = std::nextafter(.25, 1.);
    EXPECT_NO_THROW(validate_hpop_covariance(small_asymmetry));
    const auto symmetric = transport_hpop_covariance(small_asymmetry, identity_transition());
    EXPECT_EQ(symmetric.covariance.state[0][1], symmetric.covariance.state[1][0]);
}
TEST(HpopCovariance, SquareRootTransportPreservesStronglyCancellingCrossContributions) {
    auto input = identity_covariance();
    const double rho = 1 - 1e-8;
    input.drag = HpopConsiderParameter{1, {-rho, 0, 0, 0, 0, 0}};
    auto transition = identity_transition();
    // A mathematical range/cancellation case, not a physical orbit.
    transition.state_transition[0][0] = transition.drag_sensitivity[0] = 1e150;
    const auto result = transport_hpop_covariance(input, transition);
    const double expected = 2 * (1 - rho) * 1e300;
    EXPECT_LE(std::abs(result.covariance.state[0][0] - expected), expected * 3e-15);
    EXPECT_LT(result.contributions.state_parameter_cross[0][0], 0);
    EXPECT_NEAR(result.contributions.initial_state[0][0] / 1e300, 1, 1e-15);
    EXPECT_NEAR(result.contributions.consider_parameters[0][0] / 1e300, 1, 1e-15);
    EXPECT_DOUBLE_EQ(result.covariance.drag->variance, 1);
    EXPECT_NEAR(result.covariance.drag->state_cross_covariance[0] / 1e150, 1 - rho, 2e-16);
}
TEST(HpopCovariance, JointCrossCovariancesComposeAndParameterVariancesStayConstant) {
    auto input = identity_covariance();
    input.drag = HpopConsiderParameter{.04, {.02, -.01, 0, .01, 0, 0}};
    input.srp = HpopConsiderParameter{.09, {-.03, .01, 0, 0, .01, 0}};
    input.drag_srp_covariance = -.02;
    auto a = identity_transition(), b = identity_transition(), combined = identity_transition();
    for (std::size_t i = 0; i < 3; ++i) {
        a.state_transition[i][i + 3] = 2;
        b.state_transition[i][i + 3] = 3;
        combined.state_transition[i][i + 3] = 5;
    }
    for (std::size_t i = 0; i < 6; ++i) {
        a.drag_sensitivity[i] = .3 * static_cast<double>(i + 1);
        a.srp_sensitivity[i] = -.2 * static_cast<double>(i + 1);
        b.drag_sensitivity[i] = -.1 * static_cast<double>(i + 1);
        b.srp_sensitivity[i] = .4 * static_cast<double>(i + 1);
    }
    for (std::size_t i = 0; i < 6; ++i) {
        combined.drag_sensitivity[i] = b.drag_sensitivity[i];
        combined.srp_sensitivity[i] = b.srp_sensitivity[i];
        for (std::size_t j = 0; j < 6; ++j) {
            combined.drag_sensitivity[i] += b.state_transition[i][j] * a.drag_sensitivity[j];
            combined.srp_sensitivity[i] += b.state_transition[i][j] * a.srp_sensitivity[j];
        }
    }
    const auto first = transport_hpop_covariance(input, a);
    const auto split = transport_hpop_covariance(first.covariance, b);
    const auto whole = transport_hpop_covariance(input, combined);
    for (std::size_t i = 0; i < 6; ++i) {
        EXPECT_NEAR(split.covariance.drag->state_cross_covariance[i],
                    whole.covariance.drag->state_cross_covariance[i], 1e-15);
        EXPECT_NEAR(split.covariance.srp->state_cross_covariance[i],
                    whole.covariance.srp->state_cross_covariance[i], 1e-15);
        for (std::size_t j = 0; j < 6; ++j) {
            EXPECT_NEAR(split.covariance.state[i][j], whole.covariance.state[i][j], 5e-14);
            const auto &c = whole.contributions;
            EXPECT_NEAR(c.initial_state[i][j] + c.consider_parameters[i][j] +
                            c.state_parameter_cross[i][j],
                        whole.covariance.state[i][j], 5e-14);
        }
    }
    EXPECT_EQ(whole.covariance.drag->variance, input.drag->variance);
    EXPECT_EQ(whole.covariance.srp->variance, input.srp->variance);
    EXPECT_EQ(whole.covariance.drag_srp_covariance, input.drag_srp_covariance);
}
TEST(HpopCovariance, NonfiniteSingularAndOverflowingOutputsFailAndCancellationIsReusable) {
    const auto input = identity_covariance();
    auto t = identity_transition();
    t.state_transition[0][0] = 0;
    EXPECT_THROW((void)transport_hpop_covariance(input, t), PropagationError);
    t = identity_transition();
    t.state_transition[0][0] = 1e308;
    EXPECT_THROW((void)transport_hpop_covariance(input, t), PropagationError);
    t = identity_transition();
    t.drag_sensitivity[0] = std::numeric_limits<double>::quiet_NaN();
    EXPECT_THROW((void)transport_hpop_covariance(input, t), PropagationError);
    t = identity_transition();
    t.state.frame = FrameId::itrf;
    EXPECT_THROW((void)transport_hpop_covariance(input, t), std::invalid_argument);
    t = identity_transition();
    int calls = 0;
    EXPECT_THROW((void)transport_hpop_covariance(input, t,
                                                 [&] {
                                                     if (++calls == 2)
                                                         throw std::runtime_error("cancel");
                                                 }),
                 std::runtime_error);
    EXPECT_EQ(calls, 2);
    EXPECT_NO_THROW((void)transport_hpop_covariance(input, t));
    HpopOptions options;
    options.gravity_degree = options.gravity_order = 0;
    EXPECT_THROW((void)propagate_hpop_covariance(t.state, Epoch{100000000000}, {}, options),
                 std::invalid_argument);
    calls = 0;
    EXPECT_THROW((void)propagate_hpop_covariance(t.state, Epoch{100000000000}, input, options, {},
                                                 {},
                                                 [&] {
                                                     if (++calls == 20)
                                                         throw std::runtime_error("cancel");
                                                 }),
                 std::runtime_error);
    EXPECT_EQ(calls, 20);
    const auto initial = propagate_hpop_covariance(t.state, t.state.epoch, input, options);
    EXPECT_EQ(initial.covariance, input);
}

TEST(HpopCovariance, PropagationCapturesCovarianceBeforeInvokingCallbacks) {
    auto input = identity_covariance();
    const auto initial = identity_transition().state;
    HpopOptions options;
    options.gravity_degree = options.gravity_order = 0;
    const auto result = propagate_hpop_covariance(initial, initial.epoch, input, options, {}, {},
                                                  [&] { input.state = {}; });
    EXPECT_EQ(result.covariance, identity_covariance());
    EXPECT_EQ(input.state, HpopStateCovariance{});
}

TEST(HpopCovarianceSampling, BothDirectionsCancellationFailureAndInputCapture) {
    const auto initial = identity_transition().state;
    auto covariance = identity_covariance();
    HpopOptions options;
    options.gravity_degree = options.gravity_order = 0;
    std::vector<Epoch> times{Epoch{31'000'000'001},  Epoch{-80'000'000'001}, Epoch{0},
                             Epoch{-20'000'000'000}, Epoch{10'000'000'000},  Epoch{31'000'000'001}};
    const auto saved = times;
    std::vector<HpopCovarianceState> results(times.size(), HpopCovarianceState{{initial}, {}, {}});
    std::size_t callbacks = 0;
    sample_hpop_covariance(
        initial, times, covariance,
        [&](std::size_t index, const HpopCovarianceState &value) {
            ++callbacks;
            results.at(index) = value;
            times.clear();
            covariance = {};
        },
        options);
    EXPECT_EQ(callbacks, saved.size());
    for (std::size_t index = 0; index < saved.size(); ++index) {
        const auto exact =
            propagate_hpop_covariance(initial, saved[index], identity_covariance(), options);
        EXPECT_EQ(results[index].variational.state.epoch, saved[index]);
        for (std::size_t i = 0; i < 6; ++i)
            for (std::size_t j = 0; j < 6; ++j)
                EXPECT_LE(
                    std::abs(results[index].covariance.state[i][j] - exact.covariance.state[i][j]) /
                        std::sqrt(exact.covariance.state[i][i] * exact.covariance.state[j][j]),
                    3e-9);
    }
    EXPECT_EQ(results.front().covariance, results.back().covariance);
    callbacks = 0;
    const auto consume = [&](std::size_t, const HpopCovarianceState &) { ++callbacks; };
    EXPECT_THROW(sample_hpop_covariance(initial,
                                        {Epoch{1}, Epoch{std::numeric_limits<std::int64_t>::min()}},
                                        identity_covariance(), consume, options),
                 PropagationError);
    EXPECT_EQ(callbacks, 0U); // Preflight both directions before streaming anything.
    int checkpoints = 0;
    EXPECT_THROW(sample_hpop_covariance(initial, saved, identity_covariance(), consume, options, {},
                                        {},
                                        [&] {
                                            if (++checkpoints == 40)
                                                throw std::runtime_error("cancelled");
                                        }),
                 std::runtime_error);
    EXPECT_EQ(checkpoints, 40);
    EXPECT_NO_THROW(
        sample_hpop_covariance(initial, saved, identity_covariance(), consume, options));
    EXPECT_THROW(sample_hpop_covariance(
                     initial, saved, identity_covariance(),
                     [](std::size_t, const HpopCovarianceState &) {
                         throw std::runtime_error("consumer failed");
                     },
                     options),
                 std::runtime_error);
    EXPECT_THROW(sample_hpop_covariance(initial, std::vector<Epoch>(200001, Epoch{0}),
                                        identity_covariance(), consume, options),
                 std::invalid_argument);
    EXPECT_THROW(sample_hpop_covariance(initial, saved, identity_covariance(), {}, options),
                 std::invalid_argument);
}

TEST(HpopCovarianceSampling, SharedPrefixWorkReplacesRepeatedFullCoasts) {
    HpopOptions options;
    options.gravity_degree = options.gravity_order = 0;
    const auto initial = identity_transition().state;
    const auto covariance = identity_covariance();
    std::vector<Epoch> times;
    for (std::int64_t second = 0; second <= 3600; second += 30)
        times.emplace_back(second * 1'000'000'000);
    std::size_t direct_work = 0, batch_work = 0, samples = 0;
    const auto direct_start = std::chrono::steady_clock::now();
    for (auto epoch : times)
        (void)propagate_hpop_covariance(initial, epoch, covariance, options, {}, {},
                                        [&] { ++direct_work; });
    const auto batch_start = std::chrono::steady_clock::now();
    sample_hpop_covariance(
        initial, times, covariance, [&](std::size_t, const HpopCovarianceState &) { ++samples; },
        options, {}, {}, [&] { ++batch_work; });
    const auto finish = std::chrono::steady_clock::now();
    EXPECT_EQ(samples, times.size());
    EXPECT_LT(batch_work * 8, direct_work);
    RecordProperty("direct_checkpoints", static_cast<int>(direct_work));
    RecordProperty("batch_checkpoints", static_cast<int>(batch_work));
    RecordProperty("direct_seconds",
                   std::chrono::duration<double>(batch_start - direct_start).count());
    RecordProperty("batch_seconds", std::chrono::duration<double>(finish - batch_start).count());
}
