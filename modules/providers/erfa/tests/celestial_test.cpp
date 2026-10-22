#include <algorithm>
#include <cmath>
#include <erfa.h>
#include <fstream>
#include <gtest/gtest.h>
#include <limits>
#include <orbelle/core/propagation/hpop_covariance.hpp>
#include <orbelle/providers/erfa/provider.hpp>
#include <sstream>
using namespace orbelle;
namespace {
core::Matrix3 multiply(const core::Matrix3 &a, const core::Matrix3 &b) {
    core::Matrix3 c{};
    for (std::size_t i = 0; i < 3; ++i)
        for (std::size_t j = 0; j < 3; ++j)
            for (std::size_t k = 0; k < 3; ++k)
                c[i][j] += a[i][k] * b[k][j];
    return c;
}
} // namespace
TEST(CelestialFrames, MatchesOrekitIers1996AndAutomaticRates) {
    const auto frames = providers::erfa::make_frame_provider();
    const auto time = providers::erfa::make_builtin_time_provider();
    std::ifstream file(std::string(ORBELLE_REFERENCE_DIR) + "/hpop_celestial_frames.txt");
    ASSERT_TRUE(file);
    std::string line;
    int count = 0;
    double max_full = 0, max_fk5 = 0, max_rate = 0;
    while (std::getline(file, line)) {
        if (line.empty() || line[0] == '#')
            continue;
        std::istringstream in(line);
        std::int64_t ns{};
        int kind{};
        in >> ns >> kind;
        core::Epoch epoch(ns);
        core::EarthOrientationState eop{{epoch, 0, 0, 0, 0, 0, "orekit-reference"}, {}};
        in >> eop.sample.ut1_minus_tai_seconds >> eop.sample.polar_motion_x_rad >>
            eop.sample.polar_motion_y_rad >> eop.sample.celestial_pole_dx_rad >>
            eop.sample.celestial_pole_dy_rad >> eop.rates.ut1_minus_tai_seconds_per_second >>
            eop.rates.polar_motion_x_rad_per_second >> eop.rates.polar_motion_y_rad_per_second >>
            eop.rates.celestial_pole_dx_rad_per_second >>
            eop.rates.celestial_pole_dy_rad_per_second;
        core::Matrix3 expected{}, rate{}, factor{};
        for (auto *m : {&expected, &rate, &factor})
            for (auto &row : *m)
                for (auto &v : row)
                    in >> v;
        ASSERT_TRUE(in);
        const auto target = kind == 0 ? core::FrameId::fk5_j2000 : core::FrameId::true_of_date;
        SCOPED_TRACE(line.substr(0, 25));
        const auto actual = frames->gcrf_to_celestial_state(epoch, eop, target);
        // Isolate the newly introduced FK5 factor using the existing ERFA CIP.
        // Full Orekit comparison separately includes its truncated IERS XY series.
        const auto tt = time->julian_date(epoch, core::TimeScale::tt);
        double x{}, y{}, s{}, raw[3][3]{};
        eraXys06a(tt.day, tt.fraction, &x, &y, &s);
        eraC2ixys(x + eop.sample.celestial_pole_dx_rad, y + eop.sample.celestial_pole_dy_rad, s,
                  raw);
        core::Matrix3 c{};
        for (std::size_t i = 0; i < 3; ++i)
            for (std::size_t j = 0; j < 3; ++j)
                c[i][j] = raw[i][j];
        const auto isolated = multiply(factor, c);
        for (std::size_t i = 0; i < 3; ++i)
            for (std::size_t j = 0; j < 3; ++j) {
                const double full = std::abs(actual.rotation.matrix[i][j] - expected[i][j]);
                const double fk5 = std::abs(actual.rotation.matrix[i][j] - isolated[i][j]);
                const double dr = std::abs(actual.rotation_rate_per_second[i][j] - rate[i][j]);
                max_full = std::max(max_full, full);
                max_fk5 = std::max(max_fk5, fk5);
                max_rate = std::max(max_rate, dr);
                EXPECT_LE(full,
                          2e-11);      // Different equivalent IAU2006/2000A series representations.
                EXPECT_LE(fk5, 2e-12); // Same IERS 1996 model, independent Orekit evaluation.
                EXPECT_LE(dr, 2e-15);  // AD reference; no finite-difference reference rates.
            }
        const core::CartesianState state{epoch,
                                         core::OriginId::earth,
                                         core::FrameId::gcrf,
                                         {7e6, -2e6, 1e6},
                                         {-1200, 7000, 800}};
        const auto converted = core::transform_state(state, target, *frames, eop);
        const auto back = core::transform_state(converted, core::FrameId::gcrf, *frames, eop);
        const auto fixed = core::transform_state(converted, core::FrameId::itrf, *frames, eop);
        const auto fixed_direct = core::transform_state(state, core::FrameId::itrf, *frames, eop);
        for (std::size_t i = 0; i < 3; ++i) {
            EXPECT_NEAR(back.position_m[i], state.position_m[i], 1e-8);
            EXPECT_NEAR(back.velocity_mps[i], state.velocity_mps[i], 2e-11);
            EXPECT_NEAR(fixed.position_m[i], fixed_direct.position_m[i], 2e-8);
            EXPECT_NEAR(fixed.velocity_mps[i], fixed_direct.velocity_mps[i], 2e-11);
        }
        ++count;
    }
    EXPECT_EQ(count, 48);
    RecordProperty("maximum_full_rotation_error", max_full);
    RecordProperty("maximum_fk5_rotation_error", max_fk5);
    RecordProperty("maximum_rate_error", max_rate);
}
TEST(CelestialFrames, PolarMotionCancelsAndInvalidDataFails) {
    const auto frames = providers::erfa::make_frame_provider();
    const core::Epoch epoch(0);
    core::EarthOrientationState eop{{epoch, -32, 0, 0, 0, 0, "synthetic"}, {}};
    for (auto target : {core::FrameId::fk5_j2000, core::FrameId::true_of_date}) {
        const auto base = frames->gcrf_to_celestial_state(epoch, eop, target);
        auto polar = eop;
        polar.sample.polar_motion_x_rad = .01;
        polar.sample.polar_motion_y_rad = -.02;
        polar.rates.polar_motion_x_rad_per_second = 1e-5;
        EXPECT_EQ(base.rotation.matrix,
                  frames->gcrf_to_celestial_state(epoch, polar, target).rotation.matrix);
        EXPECT_EQ(base.rotation_rate_per_second,
                  frames->gcrf_to_celestial_state(epoch, polar, target).rotation_rate_per_second);
        auto bad = eop;
        bad.sample.source_id = "";
        EXPECT_THROW((void)frames->gcrf_to_celestial_state(epoch, bad, target),
                     std::invalid_argument);
        bad = eop;
        bad.rates.celestial_pole_dx_rad_per_second = std::numeric_limits<double>::quiet_NaN();
        EXPECT_THROW((void)frames->gcrf_to_celestial_state(epoch, bad, target),
                     std::invalid_argument);
        bad = eop;
        bad.sample.epoch = core::Epoch(-89726370000000000LL);
        EXPECT_NO_THROW((void)frames->gcrf_to_celestial(bad.sample.epoch, bad.sample, target));
        EXPECT_THROW((void)frames->gcrf_to_celestial_state(bad.sample.epoch, bad, target),
                     std::invalid_argument);
    }
    EXPECT_THROW((void)frames->gcrf_to_celestial(epoch, eop.sample, core::FrameId::spice_j2000),
                 std::invalid_argument);
}

namespace {
using Joint = std::array<std::array<double, 8>, 8>;
class EndpointEop final : public core::IEarthOrientationProvider {
  public:
    explicit EndpointEop(core::EarthOrientationState state)
        : state_(std::move(state)),
          info_{"endpoint",          "hash", "source", "frame", "linear", state_.sample.epoch,
                state_.sample.epoch, {},     0} {}
    core::EarthOrientationState at(core::Epoch epoch) const override {
        if (epoch != state_.sample.epoch)
            throw core::EopCoverageError("Outside endpoint");
        ++queries;
        return state_;
    }
    const core::EopDataInfo &data_info() const noexcept override { return info_; }
    mutable int queries = 0;

  private:
    core::EarthOrientationState state_;
    core::EopDataInfo info_;
};
Joint joint(const core::HpopCovarianceInput &value) {
    Joint p{};
    for (std::size_t i = 0; i < 6; ++i)
        for (std::size_t j = 0; j < 6; ++j)
            p[i][j] = value.state[i][j];
    for (const auto &[k, parameter] :
         {std::pair{std::size_t{6}, value.drag}, std::pair{std::size_t{7}, value.srp}}) {
        if (parameter) {
            p[k][k] = parameter->variance;
            for (std::size_t i = 0; i < 6; ++i)
                p[k][i] = p[i][k] = parameter->state_cross_covariance[i];
        }
    }
    p[6][7] = p[7][6] = value.drag_srp_covariance;
    return p;
}
} // namespace
TEST(CelestialFrames, JointCovarianceMatchesIndependentReferencesAtCoverageEndpoint) {
    const std::shared_ptr<const core::IFrameProvider> frames =
        providers::erfa::make_frame_provider();
    std::ifstream file(std::string(ORBELLE_REFERENCE_DIR) +
                       "/../propagators/hpop_celestial_covariance.txt");
    ASSERT_TRUE(file);
    int count = 0;
    double maximum = 0;
    std::string line;
    while (std::getline(file, line)) {
        if (line.empty() || line[0] == '#')
            continue;
        std::istringstream in(line);
        std::int64_t ns{};
        int kind{}, rotating{};
        in >> ns >> kind >> rotating;
        core::Epoch epoch(ns);
        core::EarthOrientationState eop{{epoch, 0, 0, 0, 0, 0, "reference"}, {}};
        in >> eop.sample.ut1_minus_tai_seconds >> eop.sample.polar_motion_x_rad >>
            eop.sample.polar_motion_y_rad >> eop.sample.celestial_pole_dx_rad >>
            eop.sample.celestial_pole_dy_rad >> eop.rates.ut1_minus_tai_seconds_per_second >>
            eop.rates.polar_motion_x_rad_per_second >> eop.rates.polar_motion_y_rad_per_second >>
            eop.rates.celestial_pole_dx_rad_per_second >>
            eop.rates.celestial_pole_dy_rad_per_second;
        Joint p{}, expected{};
        for (auto *m : {&p, &expected})
            for (auto &row : *m)
                for (auto &v : row)
                    in >> v;
        ASSERT_TRUE(in);
        core::HpopCovarianceInput input;
        for (std::size_t i = 0; i < 6; ++i)
            std::copy_n(p[i].begin(), 6, input.state[i].begin());
        input.drag = core::HpopConsiderParameter{p[6][6], {}};
        input.srp = core::HpopConsiderParameter{p[7][7], {}};
        std::copy_n(p[6].begin(), 6, input.drag->state_cross_covariance.begin());
        std::copy_n(p[7].begin(), 6, input.srp->state_cross_covariance.begin());
        input.drag_srp_covariance = p[6][7];
        const auto provider = std::make_shared<EndpointEop>(eop);
        const core::HpopCovarianceFrameEnvironment environment{frames, provider};
        const core::CartesianState state{epoch,
                                         core::OriginId::earth,
                                         core::FrameId::gcrf,
                                         {7e6, 2e6, 1e6},
                                         {-1000, 7000, 1000}};
        const core::HpopCovarianceCoordinates target{
            kind == 0 ? core::HpopCovarianceFrame::fk5_j2000
                      : core::HpopCovarianceFrame::true_of_date,
            rotating ? core::HpopCovarianceVelocity::rotating
                     : core::HpopCovarianceVelocity::projected};
        const auto output =
            core::convert_hpop_covariance(state, input, target, {}, {}, environment);
        EXPECT_EQ(provider->queries, 1);
        const auto actual = joint(output);
        for (std::size_t i = 0; i < 8; ++i)
            for (std::size_t j = 0; j < 8; ++j) {
                const auto error = std::abs(actual[i][j] - expected[i][j]) /
                                   std::sqrt(expected[i][i] * expected[j][j]);
                maximum = std::max(maximum, error);
                EXPECT_LE(error, 1e-10);
            }
        EXPECT_EQ(output.drag->variance, input.drag->variance);
        EXPECT_EQ(output.srp->variance, input.srp->variance);
        EXPECT_EQ(output.drag_srp_covariance, input.drag_srp_covariance);
        const auto back =
            joint(core::convert_hpop_covariance(state, output, {}, {}, {}, environment));
        for (std::size_t i = 0; i < 8; ++i)
            for (std::size_t j = 0; j < 8; ++j)
                EXPECT_LE(std::abs(back[i][j] - p[i][j]) / std::sqrt(p[i][i] * p[j][j]), 2e-13);
        auto local_environment = environment;
        const auto captured = core::convert_hpop_covariance(
            state, input, target, {}, [&] { local_environment = {}; }, local_environment);
        EXPECT_EQ(captured.state, output.state);
        EXPECT_THROW((void)core::convert_hpop_covariance(state, input, target),
                     std::invalid_argument);
        ++count;
    }
    EXPECT_EQ(count, 96);
    RecordProperty("maximum_covariance_correlation_scaled_error", maximum);
}
