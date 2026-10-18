#include <algorithm>
#include <cmath>
#include <fstream>
#include <gtest/gtest.h>
#include <limits>
#include <orbitforge/core/propagation/orbital_events.hpp>
#include <orbitforge/runtime/analysis.hpp>
#include <orbitforge/runtime/gravity.hpp>
#include <orbitforge/runtime/project.hpp>
#include <orbitforge/runtime/two_body.hpp>
#include <sstream>
using namespace orbitforge;
namespace {
core::Epoch seconds(double s) {
    return core::Epoch{static_cast<std::int64_t>(std::llround(s * 1e9))};
}
application::ReportDefinition definition() {
    application::ReportDefinition d;
    d.id = d.name = "orbital-events";
    d.query.kind = application::ReportKind::orbital_events;
    d.query.object_ids = {"satellite-1"};
    d.query.event_tolerance_nanoseconds = 1000;
    d.format.time_scale = application::ReportTimeScale::tai;
    d.format.units = application::ReportUnits::si;
    return d;
}
runtime::ReportContext context(double first = 0, double last = 12000) {
    application::Project p{"Orbital events", seconds(first), seconds(last)};
    application::ProjectSatellite sat;
    sat.orbit.first = p.first;
    sat.orbit.last = p.last;
    sat.orbit.initial = {
        seconds(0), core::OriginId::earth, core::FrameId::gcrf, {7e6, 0, 0}, {0, 8000, 2000}};
    p.satellites.push_back(sat);
    return {p, {}, {}, {}};
}
struct Reference {
    std::string name;
    double first, last;
    runtime::ReportContext context;
    bool circular, equatorial;
    std::vector<std::array<double, 4>> events;
};
std::vector<Reference> references() {
    std::ifstream file(ORBITFORGE_ORBITAL_EVENTS_REFERENCE);
    if (!file)
        throw std::runtime_error("Missing orbital event reference");
    std::vector<Reference> cases;
    std::string line;
    while (std::getline(file, line)) {
        std::istringstream in(line);
        char type = 0;
        in >> type;
        if (type == 'C') {
            std::string name;
            double a, b;
            in >> name >> a >> b;
            auto c = context(a, b > a ? b : a + 1);
            for (auto &v : c.project.satellites[0].orbit.initial.position_m)
                in >> v;
            for (auto &v : c.project.satellites[0].orbit.initial.velocity_mps)
                in >> v;
            bool circular = false, equatorial = false;
            in >> circular >> equatorial;
            cases.push_back({name, a, b, std::move(c), circular, equatorial, {}});
        } else if (type == 'E') {
            std::array<double, 4> event{};
            for (auto &v : event)
                in >> v;
            cases.back().events.push_back(event);
        }
    }
    return cases;
}
} // namespace
TEST(OrbitalEvents, AllFourKindsMatchIndependentOrekitAndHalvedSteps) {
    if (!runtime::two_body_available())
        GTEST_SKIP();
    const auto cases = references();
    ASSERT_EQ(cases.size(), 12U);
    double max_time = 0, max_radius = 0, max_speed = 0;
    std::array<std::size_t, 4> counts{};
    for (auto mode :
         {core::AccessSearchMode::fixed_grid, core::AccessSearchMode::extremum_assisted})
        for (auto step : {60'000'000'000LL, 30'000'000'000LL})
            for (const auto &c : cases) {
                SCOPED_TRACE(c.name);
                SCOPED_TRACE(step);
                SCOPED_TRACE(static_cast<int>(mode));
                auto d = definition();
                d.query.step_nanoseconds = step;
                d.query.event_search_mode = mode;
                d.query.follow_scenario = false;
                d.query.first = seconds(c.first);
                d.query.last = seconds(c.last);
                const auto result = runtime::generate_report(c.context, d);
                ASSERT_EQ(result.table.rows().size(), c.events.size());
                ASSERT_TRUE(result.orbital_event_statistics);
                const auto &stats = *result.orbital_event_statistics;
                EXPECT_EQ(stats.object_count, 1U);
                EXPECT_EQ(stats.near_circular_objects, c.circular ? 1U : 0U);
                EXPECT_EQ(stats.near_equatorial_objects, c.equatorial ? 1U : 0U);
                std::array<std::size_t, 4> expected{};
                for (std::size_t i = 0; i < c.events.size(); ++i) {
                    const auto &row = result.table.rows()[i];
                    const auto &ref = c.events[i];
                    const auto kind = static_cast<std::size_t>(ref[1]);
                    ++expected[kind];
                    ++counts[kind];
                    EXPECT_EQ(std::get<std::string>(row[3]),
                              core::orbital_event_name(static_cast<core::OrbitalEventKind>(kind)));
                    const auto time =
                        std::abs(std::get<core::Epoch>(row[2]).seconds_since(seconds(0)) - ref[0]);
                    const auto radius = std::abs(std::get<double>(row[4]) - ref[2]);
                    const auto speed = std::abs(std::get<double>(row[5]) - ref[3]);
                    max_time = std::max(max_time, time);
                    max_radius = std::max(max_radius, radius);
                    max_speed = std::max(max_speed, speed);
                    EXPECT_LE(time, 1.1e-6);
                    EXPECT_LE(radius, .02);
                    EXPECT_LE(speed, 2e-5);
                }
                EXPECT_EQ(stats.counts, expected);
            }
    for (auto count : counts)
        EXPECT_GT(count, 0U);
    std::ostringstream metrics;
    metrics.precision(17);
    metrics << "time_s=" << max_time << "; radius_m=" << max_radius << "; speed_mps=" << max_speed;
    RecordProperty("maximum_reference_errors", metrics.str());
}
TEST(OrbitalEvents, BoundariesDegeneraciesAndInvalidInputs) {
    const auto initial = context().project.satellites[0].orbit.initial;
    const core::GravitationalParameter mu{3.986004418e14};
    const auto at = [&](core::Epoch t) {
        auto s = initial;
        s.epoch = t;
        return s;
    };
    EXPECT_TRUE(core::find_orbital_events(seconds(0), seconds(0), 1, at, mu, 1).events.empty());
    EXPECT_TRUE(
        core::find_orbital_events(seconds(0), seconds(10), 10'000'000'000, at, mu).events.empty());
    for (auto step : {0LL, -1LL})
        EXPECT_THROW((void)core::find_orbital_events(seconds(0), seconds(1), step, at, mu),
                     std::invalid_argument);
    EXPECT_THROW((void)core::find_orbital_events(seconds(1), seconds(0), 1000, at, mu, 1),
                 std::invalid_argument);
    EXPECT_THROW((void)core::find_orbital_events(seconds(0), seconds(1), 1000, at, mu, 1001),
                 std::invalid_argument);
    EXPECT_THROW((void)core::find_orbital_events(seconds(0), seconds(1), 1000, {}, mu, 1),
                 std::invalid_argument);
    EXPECT_THROW((void)core::find_orbital_events(seconds(0), seconds(1), 1000, at,
                                                 core::GravitationalParameter{0}, 1),
                 std::invalid_argument);
    EXPECT_THROW(
        (void)core::find_orbital_events(core::Epoch{std::numeric_limits<std::int64_t>::min()},
                                        core::Epoch{std::numeric_limits<std::int64_t>::max()},
                                        std::numeric_limits<std::int64_t>::max(), at, mu),
        std::invalid_argument);
    for (int fault = 0; fault < 5; ++fault) {
        const auto bad = [&](core::Epoch t) {
            auto s = at(t);
            if (fault == 0)
                s.frame = core::FrameId::itrf;
            if (fault == 1)
                s.epoch = core::Epoch{99};
            if (fault == 2)
                s.position_m[0] = std::numeric_limits<double>::quiet_NaN();
            if (fault == 3)
                s.position_m = {0, 0, 0};
            if (fault == 4)
                s.velocity_mps = {1, 0, 0};
            return s;
        };
        EXPECT_THROW(
            (void)core::find_orbital_events(seconds(0), seconds(1), 1'000'000'000, bad, mu),
            std::invalid_argument);
    }
    int calls = 0;
    EXPECT_THROW((void)core::find_orbital_events(
                     seconds(0), seconds(10), 1'000'000'000,
                     [&](core::Epoch t) {
                         if (++calls == 4)
                             throw runtime::ReportCancelled{};
                         return at(t);
                     },
                     mu),
                 runtime::ReportCancelled);
    const auto tangent = [&](core::Epoch t) {
        auto s = at(t);
        double dt = t.seconds_since(seconds(0));
        s.position_m = {7e6, 7500 * dt, dt * dt};
        s.velocity_mps = {0, 7500, 2 * dt};
        return s;
    };
    const auto touching =
        core::find_orbital_events(seconds(-10), seconds(10), 20'000'000'000, tangent, mu);
    for (const auto &event : touching.events)
        EXPECT_LT(static_cast<int>(event.kind), 2);
}
TEST(OrbitalEvents, PreparedReportsFormattingProgressAndPersistence) {
    if (!runtime::two_body_available())
        GTEST_SKIP();
    auto c = context();
    auto d = definition();
    c.project.satellites.push_back(c.project.satellites[0]);
    c.project.satellites.back().orbit.object_id = "second";
    d.query.scope = application::ReportScope::all_compatible;
    d.query.object_ids.clear();
    std::size_t previous = 0;
    const auto result = runtime::generate_report(c, d, nullptr, [&](auto completed, auto total) {
        EXPECT_GE(completed, previous);
        EXPECT_LE(completed, total);
        previous = completed;
    });
    EXPECT_EQ(previous, runtime::estimate_report_rows(c.project, d));
    ASSERT_TRUE(result.orbital_event_statistics);
    EXPECT_EQ(result.orbital_event_statistics->object_count, 2U);
    auto format = d.format;
    format.columns = {{"event.kind", "", "Event"}, {"motion.radius", "km", "Radius"}};
    const auto view = result.with_template(format);
    EXPECT_EQ(view.table.columns().size(), 2U);
    EXPECT_EQ(view.orbital_event_statistics->counts, result.orbital_event_statistics->counts);
    EXPECT_EQ(view.with_template(d.format).table.rows(), result.table.rows());
    EXPECT_DOUBLE_EQ(*view.numeric_value(0, 1), std::get<double>(result.table.rows()[0][4]) / 1000);
    application::ProjectDocument document{c.project};
    const auto analysis = runtime::prepare(document, runtime::DataSnapshot{});
    EXPECT_EQ(analysis.report(d).table.rows(), result.table.rows());
    EXPECT_THROW((void)analysis.report(d, [] { return true; }), runtime::AnalysisCancelled);
    c.project.reports.push_back(d);
    EXPECT_EQ(application::decode_project(application::encode_project(c.project)).reports[0].query,
              d.query);
    d.query.event_tolerance_nanoseconds = d.query.step_nanoseconds + 1;
    EXPECT_THROW(application::validate_report_definition(d), std::invalid_argument);
    d = definition();
    c.project.satellites[0].orbit.propagator = application::OrbitPropagator::mission;
    c.project.satellites[0].orbit.mission.segments = {
        core::MissionSegment::coast(std::chrono::seconds{12000})};
    EXPECT_THROW((void)runtime::generate_report(c, d), std::invalid_argument);
}

TEST(OrbitalEvents, HpopDenseEphemerisMatchesIndependentOrekitEvents) {
    const auto ref = references().front();
    auto c = ref.context;
    auto &input = c.project.satellites[0].orbit;
    input.propagator = application::OrbitPropagator::hpop;
    auto &options = input.hpop;
    options.gravity_degree = options.gravity_order = 0;
    options.gravity_mu = 3.986004418e14;
    core::GravityInfo info{"Orekit event point mass",
                           std::string(64, 'a'),
                           "",
                           "tide_free",
                           options.gravity_mu,
                           6378137.,
                           2,
                           0};
    const auto gravity =
        std::make_shared<core::GravityModel>(info, std::vector<core::GravityCoefficient>{});
    options.gravity_model = "event-reference";
    c.gravity.emplace(options.gravity_model, gravity);
    c.project.force_data.push_back(runtime::gravity_reference(options.gravity_model, info));
    options.integrator = core::HpopIntegrator::rkv89;
    options.relative_tolerance = 1e-14;
    options.position_tolerance_m = 1e-8;
    options.velocity_tolerance_mps = 1e-11;
    options.max_step_seconds = 30;
    options.report_ephemeris_on_fixed_step = true;
    input.step_seconds = 15;
    auto d = definition();
    const auto result = runtime::generate_report(c, d);
    ASSERT_EQ(result.table.rows().size(), ref.events.size());
    for (std::size_t i = 0; i < ref.events.size(); ++i) {
        const auto &row = result.table.rows()[i];
        EXPECT_EQ(std::get<std::string>(row[3]),
                  core::orbital_event_name(
                      static_cast<core::OrbitalEventKind>(static_cast<int>(ref.events[i][1]))));
        EXPECT_NEAR(std::get<core::Epoch>(row[2]).seconds_since(seconds(0)), ref.events[i][0],
                    2e-6);
        EXPECT_NEAR(std::get<double>(row[4]), ref.events[i][2], .02);
        EXPECT_NEAR(std::get<double>(row[5]), ref.events[i][3], 2e-5);
    }
}
TEST(OrbitalEvents, IsolatedEndpointRootsAreIncludedOnce) {
    const auto at = [](core::Epoch t) {
        const double dt = t.seconds_since(seconds(0));
        return core::CartesianState{t,
                                    core::OriginId::earth,
                                    core::FrameId::gcrf,
                                    {7e6, 8000 * dt, 2000 * dt},
                                    {0, 8000, 2000}};
    };
    for (auto mode :
         {core::AccessSearchMode::fixed_grid, core::AccessSearchMode::extremum_assisted}) {
        for (const auto &limits : {std::pair{-10., 0.}, std::pair{0., 10.}, std::pair{-10., 10.}}) {
            const auto result = core::find_orbital_events(
                seconds(limits.first), seconds(limits.second), 20'000'000'000, at,
                core::GravitationalParameter{3.986004418e14}, 1000, mode);
            ASSERT_EQ(result.events.size(), 2U);
            EXPECT_EQ(result.events[0].state.epoch, seconds(0));
            EXPECT_EQ(result.events[1].state.epoch, seconds(0));
            EXPECT_EQ(result.events[0].kind, core::OrbitalEventKind::periapsis);
            EXPECT_EQ(result.events[1].kind, core::OrbitalEventKind::ascending_node);
        }
    }
}
