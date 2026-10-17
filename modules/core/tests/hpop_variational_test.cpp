#include <algorithm>
#include <cmath>
#include <fstream>
#include <gtest/gtest.h>
#include <limits>
#include <numbers>
#include <orbitforge/core/propagation/hpop_variational.hpp>
#include <sstream>

using namespace orbitforge::core;
namespace {
using Vector = std::array<double, 6>;
using Matrix = std::array<Vector, 6>;
constexpr auto methods =
    std::array{HpopIntegrator::rk4, HpopIntegrator::rkf78, HpopIntegrator::rkv89,
               HpopIntegrator::bulirsch_stoer, HpopIntegrator::gauss_jackson};
CartesianState cartesian(const Vector &v, Epoch e = Epoch{0}) {
    return {e, OriginId::earth, FrameId::gcrf, {v[0], v[1], v[2]}, {v[3], v[4], v[5]}};
}
double scale(std::size_t i) { return i < 3 ? 1e6 : 1e3; }
HpopOptions options(HpopIntegrator method) {
    HpopOptions o;
    o.gravity_degree = o.gravity_order = 0;
    o.integrator = method;
    o.fixed_step_seconds = method == HpopIntegrator::rk4 ? 2 : 20;
    o.relative_tolerance = 1e-13;
    o.position_tolerance_m = 1e-6;
    o.velocity_tolerance_mps = 1e-9;
    o.max_step_seconds = 60;
    return o;
}
} // namespace
TEST(HpopVariational, KeplerStmMatchesIndependentOrekitAcrossIntegratorsAndTimeCoordinates) {
    std::ifstream input(ORBITFORGE_UNIVERSAL_KEPLER_STM_REFERENCE);
    ASSERT_TRUE(input);
    std::size_t count = 0;
    double maximum = 0, position_error = 0;
    for (std::string line; std::getline(input, line);) {
        if (line.empty() || line[0] == '#')
            continue;
        std::istringstream row(line);
        int id;
        double mu, seconds;
        Vector initial{}, expected{};
        Matrix phi{};
        row >> id >> mu >> seconds;
        for (auto &x : initial)
            row >> x;
        for (auto &x : expected)
            row >> x;
        for (auto &r : phi)
            for (auto &x : r)
                row >> x;
        ASSERT_TRUE(row);
        if (id != 9 && id != 14 && id != 17 && id != 22 && id != 57 && id != 62 && id != 65 &&
            id != 70)
            continue;
        for (auto method : methods)
            for (bool regular : {false, true})
                for (bool vop : {false, true}) {
                    if (regular && (id == 57 || id == 62))
                        continue;
                    if (vop && method != HpopIntegrator::rkf78 &&
                        method != HpopIntegrator::bulirsch_stoer)
                        continue;
                    SCOPED_TRACE(::testing::Message()
                                 << id << " method=" << static_cast<int>(method)
                                 << " regular=" << regular << " vop=" << vop);
                    auto o = options(method);
                    o.gravity_mu = mu;
                    o.vop = vop;
                    o.time_regularization = regular;
                    o.steps_per_orbit = method == HpopIntegrator::rk4 ? 6000 : 600;
                    o.regularization_exponent = 1.5;
                    const auto actual = propagate_hpop_variational(
                        cartesian(initial), Epoch{static_cast<std::int64_t>(seconds * 1e9)}, o);
                    for (std::size_t i = 0; i < 6; ++i) {
                        const double value =
                            i < 3 ? actual.state.position_m[i] : actual.state.velocity_mps[i - 3];
                        const double error = std::abs(value - expected[i]);
                        EXPECT_LE(error, i < 3 ? 5e-4 : 5e-7);
                        if (i < 3)
                            position_error = std::max(position_error, error);
                        EXPECT_DOUBLE_EQ(actual.drag_sensitivity[i], 0);
                        EXPECT_DOUBLE_EQ(actual.srp_sensitivity[i], 0);
                        for (std::size_t j = 0; j < 6; ++j) {
                            const double normalized =
                                std::abs(actual.state_transition[i][j] - phi[i][j]) * scale(j) /
                                scale(i);
                            maximum = std::max(maximum, normalized);
                            EXPECT_LE(normalized, 5e-9);
                        }
                    }
                    ++count;
                }
    }
    EXPECT_EQ(count, 98U);
    RecordProperty("cases", count);
    RecordProperty("max_scaled_stm_error", maximum);
    RecordProperty("max_position_error_m", position_error);
}
TEST(HpopVariational, InitialIdentityValidationAndCancellationDoNotPublishPartialResults) {
    const auto initial = cartesian({7e6, 1e6, 2e5, -1000, 7200, 1200});
    auto o = options(HpopIntegrator::rkf78);
    const auto value = propagate_hpop_variational(initial, initial.epoch, o);
    EXPECT_EQ(value.state.position_m, initial.position_m);
    for (std::size_t i = 0; i < 6; ++i)
        for (std::size_t j = 0; j < 6; ++j)
            EXPECT_DOUBLE_EQ(value.state_transition[i][j], i == j ? 1 : 0);
    HpopVariationalOptions v;
    v.absolute_tolerance = std::numeric_limits<double>::quiet_NaN();
    EXPECT_THROW((void)propagate_hpop_variational(initial, Epoch{1000000000}, o, {}, v),
                 std::invalid_argument);
    v = {};
    v.gravity = HpopGravityTruncation{1, 0};
    EXPECT_THROW((void)propagate_hpop_variational(initial, Epoch{1000000000}, o, {}, v),
                 std::invalid_argument);
    int calls = 0;
    EXPECT_THROW((void)propagate_hpop_variational(initial, Epoch{100000000000}, o, {}, {},
                                                  [&] {
                                                      if (++calls == 15)
                                                          throw std::runtime_error("cancel");
                                                  }),
                 std::runtime_error);
    EXPECT_EQ(calls, 15);
    EXPECT_NO_THROW((void)propagate_hpop_variational(initial, Epoch{100000000000}, o));
}

namespace {
using Vec3 = std::array<double, 3>;
using Mat3 = std::array<Vec3, 3>;
constexpr double omega = 7.292115e-5, solar_mu = 1.3271244004127942e20;
class VariationalTime final : public ITimeProvider {
    TimeDataInfo info_{"synthetic", std::string(64, 'a'), "", 0, 100000};

  public:
    Epoch from_utc(const UtcDateTime &u) const override {
        if (u.year != 2000 || u.month != 1 || u.day != 1)
            throw std::out_of_range("synthetic day");
        return Epoch{((u.hour - 12) * 3600LL + u.minute * 60LL + u.second) * 1000000000LL +
                     u.nanosecond};
    }
    UtcDateTime to_utc(Epoch e) const override {
        auto ns = e.tai_nanoseconds() + 43200000000000LL;
        if (ns < 0 || ns >= 86400000000000LL)
            throw std::out_of_range("synthetic day");
        auto sec = ns / 1000000000LL;
        return {2000,
                1,
                1,
                static_cast<int>(sec / 3600),
                static_cast<int>(sec / 60 % 60),
                static_cast<int>(sec % 60),
                static_cast<int>(ns % 1000000000LL)};
    }
    int tai_minus_utc_seconds(Epoch) const override { return 0; }
    JulianDate julian_date(Epoch e, TimeScale s) const override {
        return {s, 2451545, e.seconds_since(Epoch{0}) / 86400};
    }
    const TimeDataInfo &data_info() const noexcept override { return info_; }
};
class VariationalEop final : public IEarthOrientationProvider {
    EopDataInfo info_{"synthetic",
                      std::string(64, 'b'),
                      "",
                      "",
                      "",
                      Epoch{-43200000000000LL},
                      Epoch{43200000000000LL},
                      VariationalTime{}.data_info()};

  public:
    EarthOrientationState at(Epoch e) const override {
        return {{e, 0, 0, 0, 0, 0, "synthetic"}, {0, 0, 0, 0, 0}};
    }
    const EopDataInfo &data_info() const noexcept override { return info_; }
};
class VariationalFrames final : public IFrameProvider {
  public:
    PositionRotation gcrf_to_itrf(Epoch e, const EarthOrientationSample &) const override {
        const double a = .3 + omega * e.seconds_since(Epoch{0}), c = std::cos(a), s = std::sin(a);
        return {e,
                OriginId::earth,
                FrameId::gcrf,
                FrameId::itrf,
                {{{c, s, 0}, {-s, c, 0}, {0, 0, 1}}},
                "rotating",
                "synthetic"};
    }
    StateTransform gcrf_to_itrf_state(Epoch e, const EarthOrientationState &value) const override {
        const auto r = gcrf_to_itrf(e, value.sample);
        const auto &m = r.matrix;
        return {r,
                {{{-omega * m[0][1], omega * m[0][0], 0},
                  {-omega * m[0][0], -omega * m[0][1], 0},
                  {0, 0, 0}}},
                "analytic"};
    }
};
class VariationalEphemeris final : public IThirdBodyEphemeris {
    ThirdBodyDataInfo info_{"synthetic",
                            std::string(64, 'c'),
                            "circular Sun",
                            Epoch{-43200000000000LL},
                            Epoch{43200000000000LL},
                            solar_mu,
                            4.902800118e12};

  public:
    const ThirdBodyDataInfo &data_info() const noexcept override { return info_; }
    Vec3 position(ThirdBody b, Epoch e) const override {
        if (b == ThirdBody::moon)
            return {3e8, -2e8, 1e8};
        if (b != ThirdBody::sun)
            throw std::invalid_argument("synthetic body");
        const double a = .3 + 2 * std::numbers::pi / (365.25 * 86400) * e.seconds_since(Epoch{0});
        return {1.496e11 * std::cos(a), 1.496e11 * std::sin(a) * std::cos(.409),
                1.496e11 * std::sin(a) * std::sin(.409)};
    }
    GeometricBodyState geometric_state(ThirdBody b, Epoch e) const override {
        constexpr double rate = 2 * std::numbers::pi / (365.25 * 86400);
        const double a = .3 + rate * e.seconds_since(Epoch{0});
        return {position(b, e),
                {-1.496e11 * rate * std::sin(a), 1.496e11 * rate * std::cos(a) * std::cos(.409),
                 1.496e11 * rate * std::cos(a) * std::sin(.409)}};
    }
    void validate_interval(Epoch a, Epoch b, std::span<const ThirdBody> bodies) const override {
        if (a.tai_nanoseconds() < info_.first.tai_nanoseconds() ||
            b.tai_nanoseconds() > info_.last.tai_nanoseconds())
            throw std::out_of_range("synthetic ephemeris");
        for (auto id : bodies)
            if (id != ThirdBody::sun && id != ThirdBody::moon)
                throw std::invalid_argument("synthetic body");
    }
};
HpopEnvironment full_environment() {
    auto gravity = std::make_shared<GravityModel>(
        GravityInfo{"variational", {}, {}, "tide_free", egm96_mu, egm96_radius_m, 2, 2},
        std::vector<GravityCoefficient>{
            {2, 0, -4.84165371736e-4, 0}, {2, 1, 3e-7, -4e-7}, {2, 2, 2e-6, -7e-7}});
    return {std::make_shared<VariationalFrames>(), std::make_shared<VariationalEop>(), gravity,
            std::make_shared<VariationalEphemeris>(), std::make_shared<VariationalTime>()};
}
HpopOptions full_options(int mask, double coefficient) {
    auto o = options(HpopIntegrator::rkf78);
    o.gravity_model = "variational";
    o.gravity_degree = o.gravity_order = 2;
    o.include_relativistic_accelerations = (mask & 1) != 0;
    o.third_body(ThirdBody::sun).enabled = o.third_body(ThirdBody::moon).enabled = (mask & 2) != 0;
    o.srp.enabled = (mask & 4) != 0;
    o.srp.sun_position_method = SrpSunPositionMethod::geometric;
    o.srp.shadow_model = (mask & 16) != 0 ? SrpShadowModel::dual_cone : SrpShadowModel::none;
    o.srp.boundary_mitigation = true;
    o.srp.moon_shadow = false;
    o.srp.sun_radius_m = 695700000;
    o.srp.cr = 1.7 * coefficient;
    o.srp.area_mass_ratio_m2_per_kg = .03;
    o.drag.enabled = (mask & 8) != 0;
    o.drag.atmosphere = AtmosphereModel::nrlmsise00;
    o.drag.cd = 2.2 * coefficient;
    o.drag.area_mass_ratio_m2_per_kg = .02;
    return o;
}
} // namespace
TEST(HpopVariational, CombinedCoastsMatchIndependentOrekitFieldDerivatives) {
    std::ifstream file(ORBITFORGE_HPOP_VARIATIONAL_REFERENCE);
    ASSERT_TRUE(file);
    double maximum = 0, max_d = 0, max_k = 0, max_position = 0;
    std::size_t count = 0, rows = 0;
    const auto env = full_environment();
    for (std::string line; std::getline(file, line);) {
        if (line.empty() || line[0] == '#')
            continue;
        int id, mask, order;
        double coefficient, seconds;
        Vector initial{}, expected{}, d{}, k{};
        Matrix phi{};
        std::istringstream row(line);
        row >> id >> mask >> coefficient >> order >> seconds;
        for (auto &x : initial)
            row >> x;
        for (auto &x : expected)
            row >> x;
        for (auto &r : phi)
            for (auto &x : r)
                row >> x;
        for (auto &x : d)
            row >> x;
        for (auto &x : k)
            row >> x;
        ASSERT_TRUE(row);
        ++rows;
        for (auto method : methods)
            for (bool regular : {false, true})
                for (bool vop : {false, true}) {
                    if (vop && method != HpopIntegrator::rkf78 &&
                        method != HpopIntegrator::bulirsch_stoer)
                        continue;
                    SCOPED_TRACE(::testing::Message()
                                 << id << " method=" << static_cast<int>(method)
                                 << " regular=" << regular << " vop=" << vop);
                    auto o = full_options(mask, coefficient);
                    o.integrator = method;
                    o.vop = vop;
                    o.time_regularization = regular;
                    o.fixed_step_seconds = method == HpopIntegrator::rk4 ? 2 : 20;
                    o.steps_per_orbit = method == HpopIntegrator::rk4 ? 6000 : 600;
                    o.correction = coefficient < 0 ? HpopCorrection::pseudo : HpopCorrection::full;
                    HpopVariationalOptions vo;
                    vo.gravity = HpopGravityTruncation{2, order};
                    const auto actual = propagate_hpop_variational(
                        cartesian(initial), Epoch{static_cast<std::int64_t>(seconds * 1e9)}, o, env,
                        vo);
                    double norm_d = 1e-7, norm_k = 1e-7, error_d = 0, error_k = 0;
                    for (std::size_t i = 0; i < 6; ++i) {
                        const double value =
                            i < 3 ? actual.state.position_m[i] : actual.state.velocity_mps[i - 3];
                        const double error = std::abs(value - expected[i]);
                        EXPECT_LE(error, i < 3 ? 1e-3 : 1e-6);
                        if (i < 3)
                            max_position = std::max(max_position, error);
                        const double unit = i < 3 ? 1 : 1000;
                        norm_d = std::max(norm_d, std::abs(d[i]) * unit);
                        norm_k = std::max(norm_k, std::abs(k[i]) * unit);
                        error_d =
                            std::max(error_d, std::abs(actual.drag_sensitivity[i] - d[i]) * unit);
                        error_k =
                            std::max(error_k, std::abs(actual.srp_sensitivity[i] - k[i]) * unit);
                        for (std::size_t j = 0; j < 6; ++j) {
                            const double error_phi =
                                std::abs(actual.state_transition[i][j] - phi[i][j]) * scale(j) /
                                scale(i);
                            EXPECT_LE(error_phi, 3e-9);
                            maximum = std::max(maximum, error_phi);
                        }
                    }
                    EXPECT_LE(error_d / norm_d, 2e-7);
                    EXPECT_LE(error_k / norm_k, 2e-7);
                    max_d = std::max(max_d, error_d / norm_d);
                    max_k = std::max(max_k, error_k / norm_k);
                    ++count;
                }
    }
    EXPECT_EQ(rows, 36U);
    EXPECT_EQ(count, 504U);
    RecordProperty("cases", count);
    RecordProperty("max_scaled_stm_error", maximum);
    RecordProperty("max_drag_relative_error", max_d);
    RecordProperty("max_srp_relative_error", max_k);
    RecordProperty("max_position_error_m", max_position);
}
TEST(HpopVariational, IndependentGravityDoesNotAlterNominalFixedStepCoast) {
    const auto initial = cartesian({7e6, 1e6, 2e5, -1000, 7200, 1200});
    auto o = full_options(15, 1);
    o.integrator = HpopIntegrator::rk4;
    o.fixed_step_seconds = 2;
    const auto env = full_environment();
    const auto end = Epoch{120000000000LL};
    const auto full = propagate_hpop_variational(initial, end, o, env);
    HpopVariationalOptions v;
    v.gravity = HpopGravityTruncation{2, 0};
    const auto truncated = propagate_hpop_variational(initial, end, o, env, v);
    EXPECT_EQ(full.state.position_m, truncated.state.position_m);
    EXPECT_EQ(full.state.velocity_mps, truncated.state.velocity_mps);
    EXPECT_NE(full.state_transition, truncated.state_transition);
    const auto nominal = propagate_hpop_ephemeris(initial, end, o, env).back().back();
    EXPECT_EQ(full.state.position_m, nominal.position_m);
    EXPECT_EQ(full.state.velocity_mps, nominal.velocity_mps);
    o.srp.shadow_model = SrpShadowModel::cylindrical;
    EXPECT_THROW((void)propagate_hpop_variational(initial, end, o, env), std::invalid_argument);
    o.srp.enabled = false;
    o.drag.atmosphere = AtmosphereModel::jacchia_roberts;
    o.drag.low_altitude_model = LowAltitudeDensityModel::nrlmsise00;
    o.drag.blending_range_m = 0;
    EXPECT_THROW((void)propagate_hpop_variational(initial, end, o, env), std::invalid_argument);
}
