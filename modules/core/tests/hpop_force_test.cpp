#include <algorithm>
#include <atomic>
#include <cmath>
#include <fstream>
#include <gtest/gtest.h>
#include <limits>
#include <orbitforge/core/propagation/hpop_force.hpp>
#include <sstream>

using namespace orbitforge::core;
namespace {
using Vector = std::array<double, 3>;
using Matrix = std::array<Vector, 3>;
constexpr double sun_mu = 1.3271244004127942e20, moon_mu = 4.902800118e12;
struct Case {
    int mask = 0;
    double cd = 0, cr = 0;
    Vector r{}, v{}, sun{}, sun_v{}, moon{}, acceleration{}, d{}, k{};
    Matrix rotation{}, rate{}, jr{}, jv{};
};
void read(std::istream &in, Vector &v) {
    for (double &x : v)
        in >> x;
}
void read(std::istream &in, Matrix &m) {
    for (auto &row : m)
        read(in, row);
}
std::vector<Case> cases() {
    std::ifstream file(ORBITFORGE_HPOP_FORCE_REFERENCE);
    if (!file)
        throw std::runtime_error("Missing combined HPOP reference");
    std::vector<Case> result;
    for (std::string line; std::getline(file, line);) {
        if (line.empty() || line[0] == '#')
            continue;
        std::istringstream in(line);
        Case c;
        in >> c.mask >> c.cd >> c.cr;
        read(in, c.r);
        read(in, c.v);
        read(in, c.rotation);
        read(in, c.rate);
        read(in, c.sun);
        read(in, c.sun_v);
        read(in, c.moon);
        read(in, c.acceleration);
        read(in, c.jr);
        read(in, c.jv);
        read(in, c.d);
        read(in, c.k);
        if (!in)
            throw std::runtime_error("Invalid combined HPOP reference");
        result.push_back(c);
    }
    return result;
}
double norm(const Vector &v) { return std::max({std::abs(v[0]), std::abs(v[1]), std::abs(v[2])}); }
double norm(const Matrix &m) { return std::max({norm(m[0]), norm(m[1]), norm(m[2])}); }
class ForceTime final : public ITimeProvider {
    TimeDataInfo info_{"synthetic-utc", std::string(64, 'a'), "", 0, 100000};

  public:
    Epoch from_utc(const UtcDateTime &) const override { return Epoch{0}; }
    UtcDateTime to_utc(Epoch) const override { return {2000, 1, 1, 12, 0, 0, 0}; }
    int tai_minus_utc_seconds(Epoch) const override { return 0; }
    JulianDate julian_date(Epoch e, TimeScale scale) const override {
        return {scale, 2451545, e.seconds_since(Epoch{0}) / 86400};
    }
    const TimeDataInfo &data_info() const noexcept override { return info_; }
};
class ForceEop final : public IEarthOrientationProvider {
    EopDataInfo info_{"synthetic-eop",
                      std::string(64, 'b'),
                      "",
                      "",
                      "",
                      Epoch{-1000000000000000000LL},
                      Epoch{1000000000000000000LL},
                      ForceTime{}.data_info()};

  public:
    EarthOrientationState at(Epoch e) const override {
        return {{e, 0, 0, 0, 0, 0, "synthetic-eop"}, {0, 0, 0, 0, 0}};
    }
    const EopDataInfo &data_info() const noexcept override { return info_; }
};
class ForceFrames final : public IFrameProvider {
    Matrix rotation_, rate_;

  public:
    explicit ForceFrames(const Case &c) : rotation_(c.rotation), rate_(c.rate) {}
    PositionRotation gcrf_to_itrf(Epoch e, const EarthOrientationSample &) const override {
        return {e,         OriginId::earth,      FrameId::gcrf,  FrameId::itrf,
                rotation_, "synthetic-rotation", "synthetic-eop"};
    }
    StateTransform gcrf_to_itrf_state(Epoch e, const EarthOrientationState &value) const override {
        return {gcrf_to_itrf(e, value.sample), rate_, "analytic-rate"};
    }
};
class ForceEphemeris final : public IThirdBodyEphemeris {
    Case c_;
    ThirdBodyDataInfo info_{"synthetic",
                            std::string(64, 'c'),
                            "fixed-geometry",
                            Epoch{-1000000000000000000LL},
                            Epoch{1000000000000000000LL},
                            sun_mu,
                            moon_mu};

  public:
    mutable std::atomic<unsigned> position_queries{0};
    explicit ForceEphemeris(const Case &c, bool invalid_mu = false) : c_(c) {
        if (invalid_mu)
            info_.sun_mu = 0;
    }
    const ThirdBodyDataInfo &data_info() const noexcept override { return info_; }
    Vector position(ThirdBody body, Epoch) const override {
        ++position_queries;
        if (body == ThirdBody::sun)
            return c_.sun;
        if (body == ThirdBody::moon)
            return c_.moon;
        throw std::invalid_argument("Unsupported synthetic body");
    }
    GeometricBodyState geometric_state(ThirdBody body, Epoch e) const override {
        if (body != ThirdBody::sun)
            throw std::invalid_argument("Unsupported synthetic state");
        return {position(body, e), c_.sun_v};
    }
    void validate_interval(Epoch a, Epoch b, std::span<const ThirdBody> bodies) const override {
        if (a.tai_nanoseconds() < info_.first.tai_nanoseconds() ||
            b.tai_nanoseconds() > info_.last.tai_nanoseconds())
            throw std::out_of_range("Synthetic ephemeris coverage");
        for (auto body : bodies)
            if (body != ThirdBody::sun && body != ThirdBody::moon)
                throw std::invalid_argument("Unsupported synthetic body");
    }
};
HpopOptions options(const Case &c) {
    HpopOptions result;
    result.gravity_model = "combined";
    result.gravity_degree = result.gravity_order = 2;
    result.include_relativistic_accelerations = (c.mask & 1) != 0;
    result.third_body(ThirdBody::sun).enabled = result.third_body(ThirdBody::moon).enabled =
        (c.mask & 2) != 0;
    result.srp.enabled = (c.mask & 4) != 0;
    result.srp.sun_position_method = SrpSunPositionMethod::geometric;
    result.srp.shadow_model = SrpShadowModel::none;
    result.srp.cr = c.cr;
    result.srp.area_mass_ratio_m2_per_kg = .03;
    result.drag.enabled = (c.mask & 8) != 0;
    result.drag.atmosphere = AtmosphereModel::nrlmsise00;
    result.drag.cd = c.cd;
    result.drag.area_mass_ratio_m2_per_kg = .02;
    return result;
}
HpopEnvironment environment(const Case &c, bool evolve = false) {
    GravityInfo info{"combined", {}, {}, "tide_free", egm96_mu, egm96_radius_m, 2, 2};
    if (evolve)
        info.secular_variations =
            GravitySecularVariations{Epoch{0}, {{2, 0, 1e-7, 0}, {2, 1, 2e-7, -3e-7}}};
    auto gravity = std::make_shared<GravityModel>(
        info, std::vector<GravityCoefficient>{
                  {2, 0, -4.84165371736e-4, 0}, {2, 1, 3e-7, -4e-7}, {2, 2, 2e-6, -7e-7}});
    return {std::make_shared<ForceFrames>(c), std::make_shared<ForceEop>(), gravity,
            std::make_shared<ForceEphemeris>(c), std::make_shared<ForceTime>()};
}
CartesianState state(const Case &c) { return {Epoch{0}, OriginId::earth, FrameId::gcrf, c.r, c.v}; }
} // namespace

TEST(HpopForce, CombinedAndIsolatedTermsMatchIndependentOrekitAutomaticDifferentiation) {
    const auto inputs = cases();
    ASSERT_EQ(inputs.size(), 216U);
    Matrix base{};
    double max_r = 0, max_v = 0, max_d = 0, max_k = 0;
    for (std::size_t row = 0; row < inputs.size(); ++row) {
        const auto &c = inputs[row];
        SCOPED_TRACE(row);
        const auto o = options(c);
        const auto e = environment(c);
        const auto s = state(c);
        const auto actual = hpop_force_differential(s, o, e);
        EXPECT_EQ(actual.force.acceleration_mps2, evaluate_hpop_acceleration(s, o, e));
        if (c.mask == 0)
            base = c.jr;
        Matrix perturbation = c.jr;
        for (unsigned i = 0; i < 3; ++i)
            for (unsigned j = 0; j < 3; ++j)
                perturbation[i][j] -= base[i][j];
        const double r_bound = 3e-18 + ((c.mask & 8) ? 2e-7 * norm(perturbation) : 0);
        for (unsigned i = 0; i < 3; ++i) {
            EXPECT_NEAR(actual.force.acceleration_mps2[i], c.acceleration[i], 3e-12);
            EXPECT_NEAR(actual.drag_parameter_jacobian_pa[i], c.d[i], 3e-11 * norm(c.d) + 1e-24);
            EXPECT_NEAR(actual.srp_parameter_jacobian_pa[i], c.k[i], 3e-13 * norm(c.k) + 1e-24);
            max_d = std::max(max_d, std::abs(actual.drag_parameter_jacobian_pa[i] - c.d[i]) /
                                        (norm(c.d) + 1e-30));
            max_k = std::max(max_k, std::abs(actual.srp_parameter_jacobian_pa[i] - c.k[i]) /
                                        (norm(c.k) + 1e-30));
            for (unsigned j = 0; j < 3; ++j) {
                const auto er = std::abs(actual.force.position_jacobian_per_s2[i][j] - c.jr[i][j]);
                const auto ev = std::abs(actual.force.velocity_jacobian_per_s[i][j] - c.jv[i][j]);
                EXPECT_LE(er, r_bound);
                EXPECT_LE(ev, 3e-11 * norm(c.jv) + 1e-24);
                max_r = std::max(max_r, er);
                max_v = std::max(max_v, ev / (norm(c.jv) + 1e-30));
            }
        }
    }
    RecordProperty("max_position_jacobian_absolute_error", max_r);
    RecordProperty("max_velocity_jacobian_relative_error", max_v);
    RecordProperty("max_D_parameter_relative_error", max_d);
    RecordProperty("max_K_parameter_relative_error", max_k);
}
TEST(HpopForce, ResourceValidationDerivativeFailuresAndCancellationPreserveInputs) {
    auto c = cases().at(23);
    auto o = options(c);
    auto e = environment(c);
    auto s = state(c);
    const auto expected = hpop_force_differential(s, o, e);
    unsigned checks = 0;
    EXPECT_THROW((void)hpop_force_differential(s, o, e,
                                               [&] {
                                                   if (++checks == 2)
                                                       throw std::runtime_error("cancel");
                                               }),
                 std::runtime_error);
    EXPECT_EQ(checks, 2U);
    EXPECT_EQ(hpop_force_differential(s, o, e).force.acceleration_mps2,
              expected.force.acceleration_mps2);
    s.frame = FrameId::itrf;
    EXPECT_THROW((void)evaluate_hpop_acceleration(s, o, e), std::invalid_argument);
    s = state(c);
    s.velocity_mps[1] = std::numeric_limits<double>::quiet_NaN();
    EXPECT_THROW((void)hpop_force_differential(s, o, e), PropagationError);
    s = state(c);
    s.position_m = e.frames->gcrf_to_itrf(s.epoch, e.eop->at(s.epoch).sample)
                       .inverse_position({0, 0, egm96_radius_m + 400000});
    EXPECT_NO_THROW((void)evaluate_hpop_acceleration(s, o, e));
    EXPECT_THROW((void)hpop_force_differential(s, o, e), PropagationError);
    e.time.reset();
    EXPECT_THROW((void)evaluate_hpop_acceleration(state(c), o, e), std::invalid_argument);
    o = HpopOptions{};
    o.gravity_degree = o.gravity_order = 0;
    e = HpopEnvironment{};
    const auto central = hpop_force_differential(state(c), o, e);
    EXPECT_EQ(central.force.velocity_jacobian_per_s, Matrix{});
    EXPECT_EQ(central.drag_parameter_jacobian_pa, Vector{});
    EXPECT_EQ(central.srp_parameter_jacobian_pa, Vector{});
    EXPECT_FALSE(e.gravity); // Built-in resolution only changes the private copy.
}
TEST(HpopForce, SecularAndSolidOceanCorrectionsRemainInTheRotatedHessian) {
    auto c = cases().at(5);
    auto o = options(c);
    auto e = environment(c, true);
    auto s = state(c);
    o.drag.enabled = o.srp.enabled = o.include_relativistic_accelerations = false;
    o.third_body(ThirdBody::sun).enabled = o.third_body(ThirdBody::moon).enabled = false;
    o.include_secular_variations = true;
    o.solid_tides.mode = SolidTideMode::full;
    o.solid_tides.truncate_to_gravity = false;
    o.ocean_tides.enabled = true;
    o.ocean_tides.degree = 4;
    o.ocean_tides.order = 2;
    OceanTideConstituent wave{55555, {}};
    for (int n = 2; n <= 4; ++n)
        for (int m = 0; m <= 2; ++m)
            wave.coefficients.push_back({n, m, n * 1e-5, 4e-6, 2e-6, 1e-6});
    e.ocean_tides = std::make_shared<OceanTideModel>(OceanTideInfo{"constant", {}, {}, 4, 2},
                                                     std::vector{wave});
    s.epoch = Epoch{315576000000000000LL};
    const auto actual = hpop_force_differential(s, o, e);
    const auto nominal = evaluate_hpop_acceleration(s, o, e);
    EXPECT_EQ(actual.force.acceleration_mps2, nominal);
    const auto difference = [&](double h) {
        Matrix result{};
        constexpr std::array weights{4. / 5, -1. / 5, 4. / 105, -1. / 280};
        for (unsigned j = 0; j < 3; ++j)
            for (unsigned k = 0; k < weights.size(); ++k) {
                auto plus = s, minus = s;
                plus.position_m[j] += (k + 1) * h;
                minus.position_m[j] -= (k + 1) * h;
                const auto a = evaluate_hpop_acceleration(plus, o, e),
                           b = evaluate_hpop_acceleration(minus, o, e);
                for (unsigned i = 0; i < 3; ++i)
                    result[i][j] += weights[k] * (a[i] - b[i]) / h;
            }
        return result;
    };
    const auto fine = difference(500), coarse = difference(1000);
    for (unsigned i = 0; i < 3; ++i)
        for (unsigned j = 0; j < 3; ++j) {
            EXPECT_NEAR(fine[i][j], coarse[i][j], 3e-17);
            EXPECT_NEAR(actual.force.position_jacobian_per_s2[i][j], fine[i][j], 3e-17);
        }
    auto off = o;
    off.solid_tides.mode = SolidTideMode::none;
    off.ocean_tides.enabled = false;
    off.include_secular_variations = false;
    const auto baseline = hpop_force_differential(s, off, e);
    EXPECT_GT(std::abs(actual.force.position_jacobian_per_s2[0][0] -
                       baseline.force.position_jacobian_per_s2[0][0]),
              1e-10);
}

TEST(HpopForce, InvalidEphemerisGravityIsRejectedBeforePositionQueries) {
    auto c = cases().front();
    c.mask = 2;
    const auto o = options(c);
    auto e = environment(c);
    const auto ephemeris = std::make_shared<ForceEphemeris>(c, true);
    e.ephemeris = ephemeris;
    EXPECT_THROW((void)evaluate_hpop_acceleration(state(c), o, e), std::invalid_argument);
    EXPECT_EQ(ephemeris->position_queries.load(), 0U);
    EXPECT_THROW((void)hpop_force_differential(state(c), o, e), std::invalid_argument);
    EXPECT_EQ(ephemeris->position_queries.load(), 0U);
    EXPECT_THROW((void)make_hpop(state(c), o, e), std::invalid_argument);
    EXPECT_EQ(ephemeris->position_queries.load(), 0U);
}
