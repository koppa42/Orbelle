#include <algorithm>
#include <cmath>
#include <fstream>
#include <gtest/gtest.h>
#include <limits>
#include <orbelle/core/propagation/hpop_covariance.hpp>
#include <orbelle/core/propagation/hpop_force.hpp>
#include <sstream>

using namespace orbelle::core;
namespace {
using Joint = std::array<std::array<double, 8>, 8>;
HpopCovarianceCoordinates coordinate(int index) {
    constexpr std::array<HpopCovarianceCoordinates, 5> values{
        {{},
         {HpopCovarianceFrame::lvlh, HpopCovarianceVelocity::projected},
         {HpopCovarianceFrame::lvlh, HpopCovarianceVelocity::rotating},
         {HpopCovarianceFrame::frenet, HpopCovarianceVelocity::projected},
         {HpopCovarianceFrame::frenet, HpopCovarianceVelocity::rotating}}};
    return values.at(static_cast<std::size_t>(index));
}
CartesianState circular() {
    return {Epoch{0}, OriginId::earth, FrameId::gcrf, {7e6, 0, 0}, {0, 7500, 0}};
}
HpopCovarianceInput input_matrix() {
    HpopCovarianceInput result;
    for (std::size_t i = 0; i < 6; ++i)
        result.state[i][i] = i < 3 ? 1e6 : .25;
    result.drag = HpopConsiderParameter{1e-4, {.2, -.1, .3, .001, -.0002, .0003}};
    result.srp = HpopConsiderParameter{4e-4, {.1, .2, -.5, -.0001, .002, .0002}};
    result.drag_srp_covariance = 2e-5;
    return result;
}
Joint joint_matrix(const HpopCovarianceInput &input) {
    Joint result{};
    for (std::size_t i = 0; i < 6; ++i)
        std::copy(input.state[i].begin(), input.state[i].end(), result[i].begin());
    const auto add = [&](std::size_t k, const HpopConsiderParameter &p) {
        result[k][k] = p.variance;
        for (std::size_t i = 0; i < 6; ++i)
            result[k][i] = result[i][k] = p.state_cross_covariance[i];
    };
    if (input.drag)
        add(6, *input.drag);
    if (input.srp)
        add(7, *input.srp);
    result[6][7] = result[7][6] = input.drag_srp_covariance;
    return result;
}
double error(const Joint &actual, const Joint &expected) {
    double maximum = 0;
    for (std::size_t i = 0; i < 8; ++i)
        for (std::size_t j = 0; j < 8; ++j) {
            if (expected[i][i] == 0 || expected[j][j] == 0) {
                EXPECT_EQ(actual[i][j], 0);
                continue;
            }
            maximum = std::max(maximum, std::abs(actual[i][j] - expected[i][j]) /
                                            std::sqrt(expected[i][i]) / std::sqrt(expected[j][j]));
        }
    return maximum;
}
class FixedFrames final : public IFrameProvider {
  public:
    PositionRotation gcrf_to_itrf(Epoch e, const EarthOrientationSample &) const override {
        return {e,
                OriginId::earth,
                FrameId::gcrf,
                FrameId::itrf,
                {{{1, 0, 0}, {0, 1, 0}, {0, 0, 1}}},
                "fixed-test-axes",
                "synthetic"};
    }
    StateTransform gcrf_to_itrf_state(Epoch e, const EarthOrientationState &value) const override {
        return {gcrf_to_itrf(e, value.sample), {}, "fixed-test-axes"};
    }
};
class FixedEop final : public IEarthOrientationProvider {
    EopDataInfo info_{"synthetic",
                      std::string(64, 'a'),
                      "",
                      "",
                      "",
                      Epoch{-1000000000000LL},
                      Epoch{1000000000000LL},
                      {}};

  public:
    EarthOrientationState at(Epoch e) const override {
        return {{e, 0, 0, 0, 0, 0, "synthetic"}, {0, 0, 0, 0, 0}};
    }
    const EopDataInfo &data_info() const noexcept override { return info_; }
};
} // namespace
TEST(HpopCovarianceFrames, MatchesOrekitAndIndependentJointMatrixReferences) {
    std::ifstream input(ORBELLE_HPOP_COVARIANCE_FRAMES_REFERENCE);
    ASSERT_TRUE(input);
    std::string line;
    int count = 0;
    double max_jacobian = 0, max_covariance = 0, max_roundtrip = 0;
    while (std::getline(input, line)) {
        if (line.empty() || line[0] == '#')
            continue;
        std::istringstream row(line);
        int id, from, to, mask;
        row >> id >> from >> to >> mask;
        SCOPED_TRACE(id);
        auto state = circular();
        std::array<double, 3> acceleration{};
        for (auto *vector : {&state.position_m, &state.velocity_mps, &acceleration})
            for (double &value : *vector)
                row >> value;
        HpopStateCovariance expected_jacobian{};
        Joint initial{}, expected{};
        for (auto &r : expected_jacobian)
            for (double &value : r)
                row >> value;
        for (auto *matrix : {&initial, &expected})
            for (auto &r : *matrix)
                for (double &value : r)
                    row >> value;
        ASSERT_FALSE(row.fail());
        std::string extra;
        ASSERT_FALSE(row >> extra);
        const auto jacobian =
            hpop_covariance_frame_jacobian(state, coordinate(from), coordinate(to), acceleration);
        double scale = 1, difference = 0;
        for (std::size_t i = 0; i < 6; ++i)
            for (std::size_t j = 0; j < 6; ++j) {
                const double weight = i >= 3 && j < 3 ? 1000 : 1;
                difference = std::max(difference,
                                      weight * std::abs(jacobian[i][j] - expected_jacobian[i][j]));
                scale = std::max(scale, weight * std::abs(expected_jacobian[i][j]));
            }
        max_jacobian = std::max(max_jacobian, difference / scale);
        EXPECT_LE(difference / scale, 3e-13);
        HpopCovarianceInput covariance;
        covariance.coordinates = coordinate(from);
        for (std::size_t i = 0; i < 6; ++i)
            std::copy_n(initial[i].begin(), 6, covariance.state[i].begin());
        const auto parameter = [&](std::size_t k) {
            HpopConsiderParameter result;
            result.variance = initial[k][k];
            std::copy_n(initial[k].begin(), 6, result.state_cross_covariance.begin());
            return result;
        };
        if (mask & 1)
            covariance.drag = parameter(6);
        if (mask & 2)
            covariance.srp = parameter(7);
        covariance.drag_srp_covariance = initial[6][7];
        const auto actual =
            convert_hpop_covariance(state, covariance, coordinate(to), acceleration);
        EXPECT_EQ(actual.coordinates, coordinate(to));
        const double delta = error(joint_matrix(actual), expected);
        max_covariance = std::max(max_covariance, delta);
        EXPECT_LE(delta, 5e-13);
        if (covariance.drag) {
            EXPECT_EQ(actual.drag->variance, covariance.drag->variance);
        }
        if (covariance.srp) {
            EXPECT_EQ(actual.srp->variance, covariance.srp->variance);
        }
        EXPECT_EQ(actual.drag_srp_covariance, covariance.drag_srp_covariance);
        const auto back = convert_hpop_covariance(state, actual, coordinate(from), acceleration);
        const double roundtrip = error(joint_matrix(back), initial);
        max_roundtrip = std::max(max_roundtrip, roundtrip);
        EXPECT_LE(roundtrip, 2e-12);
        ++count;
    }
    EXPECT_EQ(count, 200);
    RecordProperty("cases", count);
    RecordProperty("max_jacobian_scaled_error", max_jacobian);
    RecordProperty("max_covariance_correlation_scaled_error", max_covariance);
    RecordProperty("max_roundtrip_correlation_scaled_error", max_roundtrip);
}
TEST(HpopCovarianceFrames, CircularAnalyticRateAndExplicitZeroAcceleration) {
    const auto state = circular();
    const double n = 7500. / 7e6;
    const std::array<double, 3> acceleration{-7500 * n, 0, 0};
    for (int target : {2, 4}) {
        const auto jac =
            hpop_covariance_frame_jacobian(state, {}, coordinate(target), acceleration);
        for (std::size_t i = 0; i < 6; ++i)
            for (std::size_t j = 0; j < 6; ++j) {
                double expected = i == j ? 1 : 0;
                if (i == 3 && j == 1)
                    expected = n;
                if (i == 4 && j == 0)
                    expected = -n;
                EXPECT_NEAR(jac[i][j], expected, 4e-18);
            }
    }
    const auto projected = convert_hpop_covariance(state, input_matrix(), coordinate(1));
    const auto rotating =
        convert_hpop_covariance(state, input_matrix(), coordinate(2), acceleration);
    EXPECT_NEAR(rotating.state[3][3], .25 + n * n * 1e6, 2e-15);
    EXPECT_NEAR(rotating.state[0][4], -n * 1e6, 1e-12);
    EXPECT_NEAR(projected.state[3][3], .25, 1e-15);
    // An explicitly supplied zero acceleration means straight-line motion. In
    // particular Frenet axes do not rotate; do not substitute Kepler dynamics.
    const auto straight =
        hpop_covariance_frame_jacobian(state, {}, coordinate(4), std::array<double, 3>{});
    for (std::size_t i = 3; i < 6; ++i)
        for (std::size_t j = 0; j < 3; ++j)
            EXPECT_EQ(straight[i][j], 0);
    const auto radial =
        hpop_covariance_frame_jacobian(state, {}, coordinate(2), std::array<double, 3>{});
    EXPECT_NEAR(radial[3][1], n, 2e-18);
}
TEST(HpopCovarianceFrames, UsesActualCrossTrackAccelerationAndFullInverse) {
    const auto state = circular();
    const double az = .75;
    const std::array<double, 3> acceleration{-8, .01, az};
    const auto jac = hpop_covariance_frame_jacobian(state, {}, coordinate(2), acceleration);
    EXPECT_NEAR(jac[4][2], az / 7500, 2e-18);
    EXPECT_NEAR(jac[5][1], -az / 7500, 2e-18);
    auto inclined = state;
    inclined.position_m = {7e6, 2e6, -1e6};
    inclined.velocity_mps = {2500, 6800, -1400};
    const auto input = input_matrix();
    const auto local = convert_hpop_covariance(inclined, input, coordinate(4), acceleration);
    const auto other = convert_hpop_covariance(inclined, local, coordinate(2), acceleration);
    const auto direct = convert_hpop_covariance(inclined, input, coordinate(2), acceleration);
    EXPECT_LE(error(joint_matrix(other), joint_matrix(direct)), 5e-14);
    // Rotating -> projected in the same local frame must retain the shear term.
    const auto projected = convert_hpop_covariance(inclined, local, coordinate(3), acceleration);
    const auto projected_direct = convert_hpop_covariance(inclined, input, coordinate(3));
    EXPECT_LE(error(joint_matrix(projected), joint_matrix(projected_direct)), 5e-14);
}
TEST(HpopCovarianceFrames, RejectsUndefinedAxesBadMetadataAndMissingAcceleration) {
    auto state = circular();
    EXPECT_NO_THROW((void)hpop_covariance_frame_jacobian(state, {}, coordinate(1)));
    EXPECT_THROW((void)hpop_covariance_frame_jacobian(state, {}, coordinate(2)),
                 std::invalid_argument);
    const auto a = std::array<double, 3>{-8, 0, 0};
    auto bad = coordinate(1);
    bad.frame = static_cast<HpopCovarianceFrame>(99);
    auto input = input_matrix();
    input.coordinates = bad;
    EXPECT_THROW(validate_hpop_covariance(input), std::invalid_argument);
    EXPECT_THROW((void)convert_hpop_covariance(state, input, {}, a), std::invalid_argument);
    EXPECT_THROW((void)hpop_covariance_frame_jacobian(state, {}, bad, a), std::invalid_argument);
    bad = coordinate(1);
    bad.velocity = static_cast<HpopCovarianceVelocity>(99);
    input.coordinates = bad;
    EXPECT_THROW(validate_hpop_covariance(input), std::invalid_argument);
    EXPECT_THROW((void)hpop_covariance_frame_jacobian(state, {}, bad), std::invalid_argument);
    state.velocity_mps = {7500, 0, 0};
    EXPECT_THROW((void)hpop_covariance_frame_jacobian(state, {}, coordinate(1)),
                 std::invalid_argument);
    state.velocity_mps = {7500, 1e-12, 0};
    EXPECT_THROW((void)hpop_covariance_frame_jacobian(state, {}, coordinate(1)),
                 std::invalid_argument);
    state.velocity_mps = {};
    EXPECT_THROW((void)hpop_covariance_frame_jacobian(state, {}, coordinate(1)),
                 std::invalid_argument);
    state = circular();
    state.position_m = {};
    EXPECT_THROW((void)hpop_covariance_frame_jacobian(state, {}, coordinate(3)),
                 std::invalid_argument);
    state = circular();
    state.frame = FrameId::spice_j2000;
    EXPECT_THROW((void)hpop_covariance_frame_jacobian(state, {}, coordinate(1)),
                 std::invalid_argument);
    state = circular();
    state.velocity_mps[2] = std::numeric_limits<double>::quiet_NaN();
    EXPECT_THROW((void)hpop_covariance_frame_jacobian(state, {}, coordinate(1)),
                 std::invalid_argument);
    state = circular();
    auto infinite = a;
    infinite[0] = std::numeric_limits<double>::infinity();
    EXPECT_THROW((void)hpop_covariance_frame_jacobian(state, {}, coordinate(1), infinite),
                 std::invalid_argument);
    HpopVariationalState transition{state};
    for (std::size_t i = 0; i < 6; ++i)
        transition.state_transition[i][i] = 1;
    input = input_matrix();
    input.coordinates = coordinate(1);
    EXPECT_THROW((void)transport_hpop_covariance(input, transition), std::invalid_argument);
}
TEST(HpopCovarianceFrames, CapturesInputsAndCancelsWithoutPublishingPartialConversion) {
    auto state = circular();
    auto input = input_matrix();
    const auto original = input;
    const auto expected = convert_hpop_covariance(state, input, coordinate(1));
    const auto result = convert_hpop_covariance(state, input, coordinate(1), {}, [&] {
        input.state[0][0] = -1;
        input.coordinates = coordinate(4);
        state.frame = FrameId::itrf;
    });
    EXPECT_EQ(result, expected);
    EXPECT_THROW((void)convert_hpop_covariance(circular(), original, coordinate(1), {},
                                               [] { throw std::runtime_error("cancelled"); }),
                 std::runtime_error);
    EXPECT_EQ(convert_hpop_covariance(circular(), original, coordinate(1)), expected);
}
TEST(HpopCovarianceFrames, LocalInitialCovarianceUsesNominalForceAndReturnsGcrf) {
    const auto state = circular();
    HpopOptions options;
    options.gravity_degree = 0;
    options.gravity_order = 0;
    const auto baseline =
        propagate_hpop_covariance(state, Epoch{60'000'000'000}, input_matrix(), options);
    const std::vector<Epoch> times{Epoch{-30'000'000'000}, state.epoch, Epoch{20'000'000'000},
                                   Epoch{60'000'000'000}};
    std::vector<HpopCovarianceState> samples(times.size(), HpopCovarianceState{{state}, {}, {}});
    sample_hpop_covariance(
        state, times, input_matrix(),
        [&](std::size_t i, const HpopCovarianceState &value) { samples[i] = value; }, options);
    // Built-in field's actual point-mass mu, independent of any covariance field truncation.
    const double mu = 3.986004415e14;
    const std::array<double, 3> acceleration{-mu / (7e6 * 7e6), 0, 0};
    for (int target : {1, 2, 3, 4}) {
        SCOPED_TRACE(target);
        const auto local =
            convert_hpop_covariance(state, input_matrix(), coordinate(target), acceleration);
        const auto actual = propagate_hpop_covariance(state, Epoch{60'000'000'000}, local, options);
        EXPECT_EQ(actual.covariance.coordinates, HpopCovarianceCoordinates{});
        EXPECT_LE(error(joint_matrix(actual.covariance), joint_matrix(baseline.covariance)), 2e-13);
        EXPECT_EQ(actual.variational.state.position_m, baseline.variational.state.position_m);
        EXPECT_EQ(actual.variational.state.velocity_mps, baseline.variational.state.velocity_mps);
        sample_hpop_covariance(
            state, times, local,
            [&](std::size_t i, const HpopCovarianceState &value) {
                EXPECT_EQ(value.covariance.coordinates, HpopCovarianceCoordinates{});
                EXPECT_LE(
                    error(joint_matrix(value.covariance), joint_matrix(samples[i].covariance)),
                    2e-13);
                EXPECT_EQ(value.variational.state.position_m,
                          samples[i].variational.state.position_m);
            },
            options);
    }
}

TEST(HpopCovarianceFrames, IndependentGravityTruncationDoesNotChangeAxesAcceleration) {
    auto state = circular();
    state.position_m = {7e6, 2e6, 1e6};
    state.velocity_mps = {-1000, 7000, 2000};
    HpopOptions options;
    options.gravity_degree = 2;
    options.gravity_order = 0;
    HpopEnvironment environment;
    environment.frames = std::make_shared<FixedFrames>();
    environment.eop = std::make_shared<FixedEop>();
    HpopVariationalOptions variational;
    variational.gravity = HpopGravityTruncation{0, 0};
    const auto acceleration = evaluate_hpop_acceleration(state, options, environment);
    const auto local = convert_hpop_covariance(state, input_matrix(), coordinate(4), acceleration);
    const auto actual =
        propagate_hpop_covariance(state, state.epoch, local, options, environment, variational);
    EXPECT_LE(error(joint_matrix(actual.covariance), joint_matrix(input_matrix())), 2e-13);
    options.gravity_degree = 0;
    const auto approximate = evaluate_hpop_acceleration(state, options, environment);
    const auto wrong = convert_hpop_covariance(state, local, {}, approximate);
    EXPECT_GT(error(joint_matrix(wrong), joint_matrix(input_matrix())), 1e-6);
}
