#include <cmath>
#include <future>
#include <gtest/gtest.h>
#include <orbitforge/orbitforge.hpp>
#include <orbitforge/runtime/two_body.hpp>

using namespace orbitforge;
namespace {
class BoundedMoon final : public core::IThirdBodyEphemeris {
    core::ThirdBodyDataInfo info_;

  public:
    mutable std::size_t calls = 0;
    bool queries_disabled = false;
    BoundedMoon(core::Epoch first, core::Epoch last) {
        info_.model = "bounded-test-moon";
        info_.content_sha256 = std::string(64, 'a');
        info_.interpretation_version = "test-v1";
        info_.first = first;
        info_.last = last;
        info_.moon_mu = 4.902800066e12;
    }
    std::array<double, 3> position(core::ThirdBody, core::Epoch epoch) const override {
        if (queries_disabled || epoch.tai_nanoseconds() < info_.first.tai_nanoseconds() ||
            epoch.tai_nanoseconds() > info_.last.tai_nanoseconds())
            throw core::PropagationError("Force query outside requested coast");
        ++calls;
        return {1e12, 2e11, 1e11};
    }
    void validate_interval(core::Epoch first, core::Epoch last,
                           std::span<const core::ThirdBody>) const override {
        if (first.tai_nanoseconds() < info_.first.tai_nanoseconds() ||
            last.tai_nanoseconds() > info_.last.tai_nanoseconds())
            throw core::PropagationError("Requested coverage outside coast");
    }
    const core::ThirdBodyDataInfo &data_info() const noexcept override { return info_; }
};
} // namespace
TEST(HpopEphemerisSession, BothInterpolatorsMatchCspiceForCircularAndEccentricCoasts) {
    if (!runtime::two_body_available()) {
        GTEST_SKIP();
    }
    double max_r = 0, max_v = 0;
    for (double e : {0., .72}) {
        const double radius = 7e6, speed = std::sqrt(core::egm96_mu * (1 + e) / radius);
        const core::CartesianState initial{core::Epoch{0},
                                           core::OriginId::earth,
                                           core::FrameId::gcrf,
                                           {radius, 0, 0},
                                           {0, speed * std::cos(.4), speed * std::sin(.4)}};
        const auto reference = runtime::make_two_body_session(
            initial, core::GravitationalParameter{core::egm96_mu}, "cspice");
        for (auto integrator : {HpopIntegrator::rk4, HpopIntegrator::rkf78, HpopIntegrator::rkv89,
                                HpopIntegrator::bulirsch_stoer, HpopIntegrator::gauss_jackson}) {
            for (auto sign : {-1LL, 1LL}) {
                core::HpopOptions options;
                options.gravity_degree = options.gravity_order = 0;
                options.integrator = integrator;
                options.fixed_step_seconds = integrator == HpopIntegrator::rk4 ? 2 : 10;
                options.max_step_seconds = 30;
                options.relative_tolerance = 1e-14;
                options.position_tolerance_m = 1e-8;
                options.velocity_tolerance_mps = 1e-11;
                for (auto method :
                     {OrbitInterpolationMethod::lagrange, OrbitInterpolationMethod::hermite}) {
                    for (auto report : {0LL, 30'000'000'000LL}) {
                        SCOPED_TRACE(static_cast<int>(integrator));
                        SCOPED_TRACE(e);
                        SCOPED_TRACE(sign);
                        SCOPED_TRACE(report);
                        const auto session = make_hpop_ephemeris_session(
                            initial, core::Epoch{sign * 6000'000'000'000LL}, options, {},
                            {method, 7}, report, "interpolated", 5);
                        for (auto t : {1LL, 123'123'456'789LL, 1822'123'456'789LL,
                                       5990'000'000'001LL, 6000'000'000'000LL}) {
                            const std::array epochs{core::Epoch{sign * t}};
                            const auto actual = session.sample(epochs).states()[0],
                                       expected = reference.sample(epochs).states()[0];
                            const double er =
                                std::hypot(actual.position_m[0] - expected.position_m[0],
                                           actual.position_m[1] - expected.position_m[1],
                                           actual.position_m[2] - expected.position_m[2]);
                            const double ev =
                                std::hypot(actual.velocity_mps[0] - expected.velocity_mps[0],
                                           actual.velocity_mps[1] - expected.velocity_mps[1],
                                           actual.velocity_mps[2] - expected.velocity_mps[2]);
                            EXPECT_LT(er, .005);
                            EXPECT_LT(ev, 5e-6);
                            max_r = std::max(max_r, er);
                            max_v = std::max(max_v, ev);
                        }
                    }
                }
            }
        }
    }
    RecordProperty("max_position_error_m", max_r);
    RecordProperty("max_velocity_error_mps", max_v);
}
TEST(HpopEphemerisSession, ImmutableBoundedQueriesHaveExplicitSourceAndCancellation) {
    core::HpopOptions options;
    options.gravity_degree = options.gravity_order = 0;
    const core::CartesianState initial{core::Epoch{0},
                                       core::OriginId::earth,
                                       core::FrameId::gcrf,
                                       {7e6, 0, 0},
                                       {0, std::sqrt(core::egm96_mu / 7e6), 0}};
    const auto session = make_hpop_ephemeris_session(
        initial, core::Epoch{600'000'000'000LL}, options, {},
        {OrbitInterpolationMethod::hermite, 7}, 30'000'000'000LL, "ephemeris", 9);
    EXPECT_EQ(session.provenance().object_id, "ephemeris");
    EXPECT_EQ(session.provenance().input_revision, 9U);
    EXPECT_NE(session.provenance().algorithm.find("report-ns=30000000000"), std::string::npos);
    EXPECT_NE(session.provenance().algorithm.find("method=hermite;interpolation-order=7"),
              std::string::npos);
    const std::array epochs{core::Epoch{123'000'000'001LL}, core::Epoch{0},
                            core::Epoch{600'000'000'000LL}};
    const auto before = session.sample(epochs);
    auto query = std::async(std::launch::async, [&] { return session.sample(epochs); });
    EXPECT_THROW((void)session.sample(epochs, [] { throw std::runtime_error("cancel query"); }),
                 std::runtime_error);
    EXPECT_EQ(query.get().states()[0].position_m, before.states()[0].position_m);
    EXPECT_EQ(session.sample(epochs).states()[0].position_m, before.states()[0].position_m);
    for (auto invalid : {-1LL, 600'000'000'001LL}) {
        EXPECT_THROW((void)session.sample(std::array{core::Epoch{invalid}}),
                     core::PropagationError);
    }
    EXPECT_THROW((void)make_hpop_ephemeris_session(initial, core::Epoch{1'000'000'000LL}, options,
                                                   {}, {OrbitInterpolationMethod::hermite, 7}),
                 std::invalid_argument);
    EXPECT_THROW(
        (void)make_hpop_ephemeris_session(initial, core::Epoch{600'000'000'000LL}, options, {},
                                          {OrbitInterpolationMethod::hermite, 7}, 0, "cancel", 1,
                                          [] { throw std::runtime_error("cancel creation"); }),
        std::runtime_error);
}

TEST(HpopEphemerisSession, StagesStayInTheExactCoastAndQueriesNeedNoForceProvider) {
    for (auto integrator : {HpopIntegrator::rk4, HpopIntegrator::rkf78, HpopIntegrator::rkv89,
                            HpopIntegrator::bulirsch_stoer, HpopIntegrator::gauss_jackson}) {
        for (auto sign : {-1LL, 1LL}) {
            SCOPED_TRACE(static_cast<int>(integrator));
            SCOPED_TRACE(sign);
            core::CartesianState initial{core::Epoch{8'000'000'000'000'000'000LL},
                                         core::OriginId::earth,
                                         core::FrameId::gcrf,
                                         {1e10, 0, 0},
                                         {0, std::sqrt(core::egm96_mu / 1e10), 0}};
            const core::Epoch end{initial.epoch.tai_nanoseconds() +
                                  sign * (256LL * 86400'000'000'000LL + 3)};
            auto moon = std::make_shared<BoundedMoon>((sign > 0 ? initial.epoch : end),
                                                      (sign > 0 ? end : initial.epoch));
            core::HpopEnvironment environment;
            environment.ephemeris = moon;
            core::HpopOptions options;
            options.gravity_degree = options.gravity_order = 0;
            options.third_body(core::ThirdBody::moon).enabled = true;
            options.integrator = integrator;
            options.step_control = core::HpopStepControl::fixed;
            options.fixed_step_seconds = 86400;
            for (auto report : {0LL, 86400'000'000'000LL}) {
                SCOPED_TRACE(report);
                const auto arcs =
                    propagate_hpop_ephemeris(initial, end, options, environment, report);
                EXPECT_EQ(arcs.front().front().epoch, (sign > 0 ? initial.epoch : end));
                EXPECT_EQ(arcs.back().back().epoch, (sign > 0 ? end : initial.epoch));
            }
            // A shorter coast has enough well-spaced points for order seven.
            const core::Epoch short_end{initial.epoch.tai_nanoseconds() + sign * 600'000'000'003LL};
            options.fixed_step_seconds = 10;
            const auto session = make_hpop_ephemeris_session(
                initial, short_end, options, environment, {OrbitInterpolationMethod::hermite, 7},
                30'000'000'000LL);
            const auto calls = moon->calls;
            EXPECT_GT(calls, 0U);
            moon->queries_disabled = true;
            const auto result = session.sample(std::array{
                core::Epoch{initial.epoch.tai_nanoseconds() + sign * 173'123'456'789LL}});
            EXPECT_TRUE(std::isfinite(result.states()[0].position_m[0]));
            EXPECT_EQ(moon->calls, calls);
        }
    }
}
