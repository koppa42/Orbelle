#include <algorithm>
#include <cmath>
#include <fstream>
#include <gtest/gtest.h>
#include <limits>
#include <numbers>
#include <orbitforge/core/propagation/hpop.hpp>
#include <sstream>
using namespace orbitforge::core;
namespace {
GravityInfo field(bool zero = false, int degree = 21, int order = 21) {
    return {"reference",    {},     {},   zero ? "zero_tide" : "tide_free", egm96_mu,
            egm96_radius_m, degree, order};
}
TideRaisingBody body(bool sun, double elapsed = 0, int geometry = 0) {
    const double radius = sun ? 1.496e11 : 384400000;
    const double rate = 2 * std::numbers::pi / ((sun ? 365.25 : 27.321661) * 86400);
    const double angle = (sun ? .3 : 1.2) + geometry + rate * elapsed;
    const double tilt = sun ? .409 : .5;
    return {{radius * std::cos(angle), radius * std::sin(angle) * std::cos(tilt),
             radius * std::sin(angle) * std::sin(tilt)},
            sun ? 1.3271244004127942e20 : 4.9028001184575496e12};
}
class Frames final : public IFrameProvider {
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
class Eop final : public IEarthOrientationProvider {
    EopDataInfo info_{"test",
                      "",
                      "",
                      "",
                      "",
                      Epoch{-1000000000000000},
                      Epoch{1000000000000000},
                      {"", "", "", 0, 100000}};

  public:
    EarthOrientationState at(Epoch epoch) const override {
        return {{epoch, -32, 0, 0, 0, 0, "test"}, {0, 0, 0, 0, 0}};
    }
    const EopDataInfo &data_info() const noexcept override { return info_; }
};
class Ephemeris final : public IThirdBodyEphemeris {
    ThirdBodyDataInfo info_{
        "test",        "", "test", Epoch{-90000000000000}, Epoch{90000000000000}, body(true).mu,
        body(false).mu};

  public:
    mutable std::vector<double> epochs;
    std::array<double, 3> position(ThirdBody id, Epoch epoch) const override {
        if (id != ThirdBody::sun && id != ThirdBody::moon)
            throw std::invalid_argument("Unexpected tide body");
        epochs.push_back(epoch.seconds_since(Epoch{0}));
        return body(id == ThirdBody::sun, epochs.back()).position_itrf_m;
    }
    void validate_interval(Epoch first, Epoch last,
                           std::span<const ThirdBody> bodies) const override {
        EXPECT_EQ(bodies.size(), 2U);
        EXPECT_EQ(bodies[0], ThirdBody::sun);
        EXPECT_EQ(bodies[1], ThirdBody::moon);
        if (first.tai_nanoseconds() < info_.first.tai_nanoseconds() ||
            last.tai_nanoseconds() > info_.last.tai_nanoseconds())
            throw std::out_of_range("Tide ephemeris coverage");
    }
    const ThirdBodyDataInfo &data_info() const noexcept override { return info_; }
};
} // namespace
TEST(SolidTides, FullAccelerationMatchesIndependentOrekitAcrossTruncationsAndTideSystems) {
    std::ifstream file(ORBITFORGE_SOLID_TIDE_REFERENCE);
    ASSERT_TRUE(file.is_open());
    int rows = 0;
    for (std::string line; std::getline(file, line);) {
        if (line.empty() || line[0] != 'A')
            continue;
        std::istringstream in(line);
        char tag;
        int zero, geometry, degree, order, truncate;
        std::array<double, 3> r{}, expected{};
        in >> tag >> zero >> geometry >> degree >> order >> truncate;
        for (auto &v : r)
            in >> v;
        for (auto &v : expected)
            in >> v;
        ASSERT_TRUE(in);
        auto info = field(zero != 0);
        const double c20 = -4.84165371736e-4 + (zero ? iers2003_permanent_tide_c20() : 0);
        GravityModel model(info, {{2, 0, c20, 0}});
        const std::array bodies{body(true, 0, geometry), body(false, 0, geometry)};
        const auto deltas = solid_tide_corrections(info, {SolidTideMode::full, truncate != 0},
                                                   degree, order, bodies);
        const auto actual = model.acceleration(r, degree, order, deltas);
        for (int axis = 0; axis < 3; ++axis)
            EXPECT_NEAR(actual[axis], expected[axis], 2e-14) << line;
        ++rows;
    }
    EXPECT_EQ(rows, 120);
}
TEST(SolidTides, PermanentTideMatchesAnalyticJ2AndDoesNotMutateTheBaseField) {
    auto info = field();
    GravityModel model(info, {});
    const std::array<double, 3> r{4e6, -3e6, 5e6};
    const auto unchanged = model.acceleration(r, 2, 2);
    const auto deltas = solid_tide_corrections(info, {SolidTideMode::permanent, true}, 2, 2);
    ASSERT_EQ(deltas.size(), 1U);
    EXPECT_NEAR(deltas[0].c, -4.20067548472e-9, 1e-24);
    const auto actual = model.acceleration(r, 2, 2, deltas);
    const double radius = std::hypot(r[0], r[1], r[2]);
    const double j2 = -std::sqrt(5.0) * deltas[0].c;
    const double factor = 1.5 * j2 * info.mu * info.radius_m * info.radius_m / std::pow(radius, 5);
    for (int axis = 0; axis < 3; ++axis) {
        const double analytic =
            factor * r[axis] * (5 * r[2] * r[2] / (radius * radius) - (axis == 2 ? 3 : 1));
        EXPECT_NEAR(actual[axis] - unchanged[axis], analytic, 2e-15);
    }
    EXPECT_EQ(model.acceleration(r, 2, 2), unchanged);
    const auto zero = field(true);
    EXPECT_TRUE(solid_tide_corrections(zero, {SolidTideMode::permanent, true}, 2, 2).empty());
    const auto removed = solid_tide_corrections(zero, {}, 2, 2);
    ASSERT_EQ(removed.size(), 1U);
    EXPECT_EQ(removed[0].c, -deltas[0].c);
    // A 0/0 base field contains no permanent C20 to subtract.
    EXPECT_TRUE(solid_tide_corrections(zero, {SolidTideMode::none, false}, 0, 0).empty());
    EXPECT_EQ(solid_tide_corrections(zero, {SolidTideMode::permanent, false}, 0, 0)[0].c,
              deltas[0].c);
}
TEST(SolidTides, UntruncatedCorrectionsExtendBeyondTheFileAndRemainRegularAtBodyPoles) {
    auto info = field(false, 2, 0);
    GravityModel low(info, {});
    GravityModel high(field(), {});
    std::array bodies{body(true), body(false)};
    bodies[1].position_itrf_m = {0, 0, 384400000};
    const auto polar = solid_tide_corrections(info, {SolidTideMode::full, false}, 2, 0, bodies);
    ASSERT_FALSE(polar.empty());
    EXPECT_TRUE(std::any_of(polar.begin(), polar.end(), [](auto v) { return v.degree == 4; }));
    for (const auto &r : {std::array<double, 3>{7e6, 0, 0}, std::array<double, 3>{0, 0, 7e6}}) {
        const auto a = low.acceleration(r, 0, 0, polar);
        const auto b = high.acceleration(r, 0, 0, polar);
        for (int i = 0; i < 3; ++i)
            EXPECT_EQ(a[i], b[i]);
    }
    bodies[1].position_itrf_m[0] = 1e-6;
    const auto near = solid_tide_corrections(info, {SolidTideMode::full, false}, 2, 0, bodies);
    ASSERT_EQ(near.size(), polar.size());
    for (std::size_t i = 0; i < polar.size(); ++i) {
        EXPECT_NEAR(polar[i].c, near[i].c, 1e-21);
        EXPECT_NEAR(polar[i].s, near[i].s, 1e-21);
    }
    const auto cut = solid_tide_corrections(info, {SolidTideMode::full, true}, 2, 0, bodies);
    ASSERT_EQ(cut.size(), 1U);
    EXPECT_EQ(cut[0].degree, 2);
    EXPECT_EQ(cut[0].order, 0);
}
TEST(SolidTides, HpopMatchesIndependentMovingBodyPropagationWithoutDirectThirdBodyGravity) {
    HpopOptions options;
    options.gravity_degree = options.gravity_order = 0;
    options.solid_tides = {SolidTideMode::full, false};
    options.max_step_seconds = 30;
    options.relative_tolerance = 1e-13;
    options.position_tolerance_m = 1e-5;
    options.velocity_tolerance_mps = 1e-8;
    const auto ephemeris = std::make_shared<Ephemeris>();
    HpopEnvironment environment{std::make_shared<Frames>(), std::make_shared<Eop>(), {}, ephemeris};
    CartesianState initial{Epoch{0}, OriginId::earth, FrameId::gcrf, {7e6, 0, 0}, {0, 6200, 5300}};
    EXPECT_FALSE(options.uses_third_bodies());
    EXPECT_TRUE(options.uses_ephemeris());
    const auto provider = make_hpop(initial, options, environment);
    std::ifstream file(ORBITFORGE_SOLID_TIDE_REFERENCE);
    ASSERT_TRUE(file.is_open());
    int rows = 0;
    for (std::string line; std::getline(file, line);) {
        if (line.empty() || line[0] != 'T')
            continue;
        std::istringstream in(line);
        char tag;
        long long seconds;
        std::array<double, 3> r{}, v{};
        in >> tag >> seconds;
        for (auto &x : r)
            in >> x;
        for (auto &x : v)
            in >> x;
        ASSERT_TRUE(in);
        const auto actual = provider->state_at(Epoch{seconds * 1000000000LL});
        for (int i = 0; i < 3; ++i) {
            EXPECT_NEAR(actual.position_m[i], r[i], .003) << line;
            EXPECT_NEAR(actual.velocity_mps[i], v[i], 3e-6) << line;
        }
        ++rows;
    }
    EXPECT_EQ(rows, 4);
    EXPECT_GT(ephemeris->epochs.size(), 100U);
    EXPECT_LT(*std::min_element(ephemeris->epochs.begin(), ephemeris->epochs.end()), -6000);
    EXPECT_GT(*std::max_element(ephemeris->epochs.begin(), ephemeris->epochs.end()), 6000);
    EXPECT_THROW((void)provider->state_at(Epoch{91000000000000}), std::out_of_range);
    EXPECT_EQ(provider->state_at(Epoch{6000000000000}).position_m,
              make_hpop(initial, options, environment)->state_at(Epoch{6000000000000}).position_m);
    EXPECT_THROW((void)make_hpop(initial, options, {}), std::invalid_argument);
    auto missing = environment;
    missing.eop.reset();
    EXPECT_THROW((void)make_hpop(initial, options, missing), std::invalid_argument);
    options.solid_tides.truncate_to_gravity = true;
    EXPECT_FALSE(options.uses_ephemeris());
    EXPECT_FALSE(options.uses_eop_force());
    EXPECT_NO_THROW((void)make_hpop(initial, options, {}));
}
TEST(SolidTides, InvalidModelsBodiesAndAdditiveCoefficientsFailExplicitly) {
    auto info = field();
    auto bad = SolidTideOptions{};
    bad.mode = static_cast<SolidTideMode>(99);
    EXPECT_THROW((void)solid_tide_corrections(info, bad, 2, 2), std::invalid_argument);
    EXPECT_THROW((void)solid_tide_corrections(info, {SolidTideMode::full}, 2, 2),
                 std::invalid_argument);
    info.tide_system = "mean_tide";
    EXPECT_THROW((void)solid_tide_corrections(info, {}, 2, 2), std::invalid_argument);
    info = field();
    std::array bodies{body(true), body(false)};
    bodies[0].mu = std::numeric_limits<double>::quiet_NaN();
    EXPECT_THROW((void)solid_tide_corrections(info, {SolidTideMode::full}, 2, 2, bodies),
                 std::invalid_argument);
    bodies[0] = body(true);
    bodies[1].position_itrf_m = {0, 0, 0};
    EXPECT_THROW((void)solid_tide_corrections(info, {SolidTideMode::full}, 2, 2, bodies),
                 std::invalid_argument);
    GravityModel model(info, {});
    for (const auto &values : std::vector<std::vector<GravityCoefficient>>{
             {{1, 0, 0, 0}},
             {{361, 0, 0, 0}},
             {{2, 3, 0, 0}},
             {{2, 0, 0, 1}},
             {{2, 0, 0, 0}, {2, 0, 1, 0}},
             {{2, 0, std::numeric_limits<double>::infinity(), 0}}})
        EXPECT_THROW((void)model.acceleration({7e6, 0, 0}, 2, 2, values), std::invalid_argument);
}
