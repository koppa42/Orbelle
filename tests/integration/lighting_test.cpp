#include <algorithm>
#include <cmath>
#include <fstream>
#include <gtest/gtest.h>
#include <limits>
#include <orbelle/core/propagation/lighting.hpp>
#include <orbelle/runtime/analysis.hpp>
#include <orbelle/runtime/ephemeris.hpp>
#include <orbelle/runtime/project.hpp>
#include <orbelle/runtime/two_body.hpp>
#include <sstream>
using namespace orbelle;
namespace {
core::Epoch seconds(double s) {
    return core::Epoch{static_cast<std::int64_t>(std::llround(s * 1e9))};
}
class FixedSun final : public core::IThirdBodyEphemeris {
  public:
    std::array<double, 3> sun{149597870700., 0, 0};
    core::ThirdBodyDataInfo info{"Synthetic independent reference",
                                 std::string(64, 'a'),
                                 "lighting-reference-v1",
                                 seconds(-1e6),
                                 seconds(1e6),
                                 1.3271244004127942e20,
                                 4.9028001184575496e12};
    std::array<double, 3> position(core::ThirdBody body, core::Epoch at) const override {
        if (body != core::ThirdBody::sun)
            throw std::invalid_argument("Only Sun");
        validate_interval(at, at, std::span{&body, 1});
        return sun;
    }
    void validate_interval(core::Epoch first, core::Epoch last,
                           std::span<const core::ThirdBody>) const override {
        if (first.tai_nanoseconds() < info.first.tai_nanoseconds() ||
            last.tai_nanoseconds() > info.last.tai_nanoseconds())
            throw std::out_of_range("Solar coverage");
    }
    const core::ThirdBodyDataInfo &data_info() const noexcept override { return info; }
};
application::ReportDefinition definition() {
    application::ReportDefinition d;
    d.id = d.name = "lighting";
    d.query.kind = application::ReportKind::lighting_intervals;
    d.query.object_ids = {"satellite-1"};
    d.format.time_scale = application::ReportTimeScale::tai;
    d.query.event_tolerance_nanoseconds = 1000;
    return d;
}
runtime::ReportContext context(double first = 0, double last = 6000) {
    application::Project p{"Lighting", seconds(first), seconds(last)};
    application::ProjectSatellite sat;
    sat.orbit.first = p.first;
    sat.orbit.last = p.last;
    sat.orbit.initial = {seconds(0),
                         core::OriginId::earth,
                         core::FrameId::gcrf,
                         {7e6, 0, 0},
                         {0, std::sqrt(3.986004418e14 / 7e6), 0}};
    p.satellites.push_back(sat);
    auto sun = std::make_shared<FixedSun>();
    p.force_data.push_back(runtime::ephemeris_reference("de440s", sun->data_info()));
    runtime::ReportContext c{p, {}, {}, {}};
    c.ephemerides.emplace("de440s", sun);
    return c;
}
} // namespace
TEST(Lighting, IntervalsFractionsAndStatisticsMatchIndependentOrekit) {
    if (!runtime::two_body_available())
        GTEST_SKIP();
    struct Case {
        std::string name;
        double first, last;
        runtime::ReportContext c;
        std::vector<std::array<double, 5>> intervals;
        std::vector<std::array<double, 7>> samples;
    };
    std::ifstream file(ORBELLE_LIGHTING_REFERENCE);
    ASSERT_TRUE(file);
    std::vector<Case> cases;
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
            auto sun = std::make_shared<FixedSun>();
            for (auto &v : sun->sun)
                in >> v;
            c.ephemerides["de440s"] = sun;
            cases.push_back({name, a, b, std::move(c), {}, {}});
        } else if (type == 'I') {
            std::array<double, 5> r{};
            for (auto &v : r)
                in >> v;
            cases.back().intervals.push_back(r);
        } else if (type == 'V') {
            std::array<double, 7> r{};
            for (auto &v : r)
                in >> v;
            cases.back().samples.push_back(r);
        }
    }
    ASSERT_EQ(cases.size(), 9U);
    double max_time = 0, max_fraction = 0, max_duration = 0, max_high_precision = 0;
    for (auto step : {60'000'000'000LL, 30'000'000'000LL})
        for (const auto &c : cases) {
            SCOPED_TRACE(c.name);
            SCOPED_TRACE(step);
            auto d = definition();
            d.query.step_nanoseconds = step;
            d.query.follow_scenario = false;
            d.query.first = seconds(c.first);
            d.query.last = seconds(c.last);
            const auto result = runtime::generate_report(c.c, d);
            ASSERT_EQ(result.table.rows().size(), c.intervals.size());
            ASSERT_TRUE(result.lighting_statistics);
            const auto &stats = *result.lighting_statistics;
            EXPECT_EQ(stats.object_count, 1U);
            std::array<results::LightingPhaseStatistics, 3> expected{};
            for (std::size_t i = 0; i < c.intervals.size(); ++i) {
                const auto &r = c.intervals[i];
                const auto &row = result.table.rows()[i];
                for (int j = 0; j < 2; ++j) {
                    const auto error = std::abs(
                        std::get<core::Epoch>(row[2 + j]).seconds_since(seconds(0)) - r[j]);
                    max_time = std::max(max_time, error);
                    EXPECT_LE(error, 1.1e-6);
                }
                const auto phase = static_cast<int>(r[2]);
                EXPECT_EQ(std::get<std::string>(row[5]),
                          core::lighting_phase_name(static_cast<core::LightingPhase>(phase)));
                EXPECT_EQ(std::get<std::int64_t>(row[6]), static_cast<int>(r[3]));
                EXPECT_EQ(std::get<std::int64_t>(row[7]), static_cast<int>(r[4]));
                if (i) {
                    EXPECT_EQ(std::get<core::Epoch>(row[2]),
                              std::get<core::Epoch>(result.table.rows()[i - 1][3]));
                }
                const auto duration = r[1] - r[0];
                auto &s = expected[phase];
                ++s.count;
                s.total_duration_s += duration;
                s.longest_duration_s = std::max(s.longest_duration_s, duration);
            }
            double sum = 0;
            for (int i = 0; i < 3; ++i) {
                const auto error =
                    std::abs(stats.phases[i].total_duration_s - expected[i].total_duration_s);
                max_duration = std::max(max_duration, error);
                EXPECT_EQ(stats.phases[i].count, expected[i].count);
                EXPECT_LE(error, 2.2e-6 * (expected[i].count + 1));
                EXPECT_NEAR(stats.phases[i].longest_duration_s, expected[i].longest_duration_s,
                            2.2e-6);
                sum += stats.phases[i].total_duration_s;
                const auto span = c.last - c.first;
                EXPECT_NEAR(stats.phases[i].fraction,
                            span > 0 ? expected[i].total_duration_s / span : 0,
                            span > 0 ? 2.2e-6 * (expected[i].count + 1) / span : 0);
            }
            EXPECT_NEAR(sum, c.last - c.first, 1e-9);
            for (const auto &sample : c.samples) {
                d.query.kind = application::ReportKind::lighting_state;
                d.query.follow_scenario = false;
                d.query.first = d.query.last = seconds(sample[0]);
                const auto value = runtime::generate_report(c.c, d);
                ASSERT_EQ(value.table.rows().size(), 1U);
                const auto fraction = std::get<double>(value.table.rows()[0][3]);
                const auto error = std::abs(fraction - sample[1]);
                max_fraction = std::max(max_fraction, error);
                // Orekit's double-precision lens formula loses precision at grazing contact;
                // its independently audited envelope is separate from our strict 80-digit check.
                EXPECT_LE(error, c.name == "grazing"       ? 2.5e-7
                                 : c.name == "short_umbra" ? 3e-8
                                                           : 3e-9);
                max_high_precision = std::max(max_high_precision, std::abs(fraction - sample[6]));
                EXPECT_NEAR(fraction, sample[6], 5e-11) << "t=" << sample[0];
                EXPECT_EQ(std::get<std::string>(value.table.rows()[0][4]),
                          core::lighting_phase_name(
                              static_cast<core::LightingPhase>(static_cast<int>(sample[2]))));
                EXPECT_NEAR(*value.numeric_value(0, 3), 100 * fraction, 1e-12);
            }
        }
    std::ostringstream metrics;
    metrics.precision(17);
    metrics << "boundary_s=" << max_time << "; fraction=" << max_fraction
            << "; phase_duration_s=" << max_duration
            << "; high_precision_fraction=" << max_high_precision;
    RecordProperty("maximum_reference_errors", metrics.str());
}
TEST(Lighting, GeometryDegeneraciesAndFailurePaths) {
    const std::array<double, 3> sun{149597870700., 0, 0};
    EXPECT_EQ(core::evaluate_lighting({7e6, 0, 0}, sun).phase, core::LightingPhase::sunlight);
    EXPECT_EQ(core::evaluate_lighting({-7e6, 0, 0}, sun).fraction, 0);
    const auto ring = core::evaluate_lighting({-2e9, 0, 0}, sun);
    EXPECT_EQ(ring.phase, core::LightingPhase::penumbra);
    EXPECT_GT(ring.fraction, 0);
    EXPECT_LT(ring.fraction, 1);
    const double radius_ratio =
        std::asin(6378137. / 2e9) / std::asin(695700000. / (149597870700. + 2e9));
    EXPECT_NEAR(ring.fraction, 1 - radius_ratio * radius_ratio, 1e-15);
    for (auto mode :
         {core::AccessSearchMode::fixed_grid, core::AccessSearchMode::extremum_assisted})
        EXPECT_THROW((void)core::find_lighting_intervals(
                         core::Epoch{std::numeric_limits<std::int64_t>::min()},
                         core::Epoch{std::numeric_limits<std::int64_t>::max()},
                         std::numeric_limits<std::int64_t>::max(),
                         [](auto) { return core::LightingGeometry{}; }, 1, mode),
                     std::invalid_argument);
    EXPECT_THROW((void)core::evaluate_lighting({0, 0, 0}, sun), core::PropagationError);
    EXPECT_THROW((void)core::evaluate_lighting({7e6, 0, 0}, {7e6, 0, 0}), core::PropagationError);
    EXPECT_THROW((void)core::find_lighting_intervals(seconds(0), seconds(1), 0,
                                                     [](auto) { return core::LightingGeometry{}; }),
                 std::invalid_argument);
    EXPECT_THROW((void)core::find_lighting_intervals(
                     seconds(0), seconds(1), 1'000'000,
                     [](auto) {
                         return core::LightingGeometry{core::LightingPhase::sunlight,
                                                       std::numeric_limits<double>::quiet_NaN()};
                     }),
                 std::invalid_argument);
    int calls = 0;
    EXPECT_THROW((void)core::find_lighting_intervals(seconds(0), seconds(10), 1'000'000'000,
                                                     [&](auto t) {
                                                         if (++calls == 8)
                                                             throw runtime::ReportCancelled{};
                                                         return core::LightingGeometry{
                                                             core::LightingPhase::sunlight, 1,
                                                             t.seconds_since(seconds(0)) + 1, 2};
                                                     }),
                 runtime::ReportCancelled);
}
TEST(Lighting, PreparedDataReportingTemplatesAndValidation) {
    if (!runtime::two_body_available())
        GTEST_SKIP();
    auto c = context();
    auto d = definition();
    auto missing = c;
    missing.ephemerides.clear();
    EXPECT_THROW((void)runtime::generate_report(missing, d), std::invalid_argument);
    auto wrong = c;
    wrong.project.force_data.clear();
    EXPECT_THROW((void)runtime::generate_report(wrong, d), std::invalid_argument);
    auto provider = std::make_shared<FixedSun>();
    provider->info.last = seconds(10);
    c.ephemerides["de440s"] = provider;
    c.project.force_data = {runtime::ephemeris_reference("de440s", provider->info)};
    EXPECT_THROW((void)runtime::generate_report(c, d), std::out_of_range);
    c = context();
    c.project.satellites.push_back(c.project.satellites[0]);
    c.project.satellites.back().orbit.object_id = "second";
    d.query.object_ids.clear();
    d.query.scope = application::ReportScope::all_compatible;
    std::size_t prior = 0, progress_calls = 0;
    const auto result = runtime::generate_report(c, d, nullptr, [&](auto completed, auto total) {
        EXPECT_GE(completed, prior);
        EXPECT_LE(completed, total);
        prior = completed;
        ++progress_calls;
    });
    EXPECT_GT(progress_calls, 2U);
    EXPECT_EQ(prior, runtime::estimate_report_rows(c.project, d));
    ASSERT_TRUE(result.lighting_statistics);
    EXPECT_EQ(result.lighting_statistics->object_count, 2U);
    double total = 0;
    for (const auto &s : result.lighting_statistics->phases)
        total += s.total_duration_s;
    EXPECT_NEAR(total, 12000, 1e-9);
    auto format = d.format;
    format.columns = {{"lighting.phase", "", "Phase"}};
    auto view = result.with_template(format);
    EXPECT_EQ(view.table.columns().size(), 1U);
    EXPECT_EQ(view.lighting_statistics->phases[2].count,
              result.lighting_statistics->phases[2].count);
    EXPECT_EQ(view.with_template(d.format).table.rows(), result.table.rows());
    std::atomic_bool cancel = true;
    EXPECT_THROW((void)runtime::generate_report(c, d, &cancel), runtime::ReportCancelled);
    d.query.event_tolerance_nanoseconds = d.query.step_nanoseconds + 1;
    EXPECT_THROW(application::validate_report_definition(d), std::invalid_argument);
    d = definition();
    c.project.reports.push_back(d);
    EXPECT_EQ(application::decode_project(application::encode_project(c.project)).reports[0].query,
              d.query);
}

TEST(Lighting, AnalysisCapturesSolarDataAndUsesTheSharedReportEngine) {
    if (!runtime::two_body_available())
        GTEST_SKIP();
    auto c = context();
    application::ProjectDocument document{c.project};
    const auto data = runtime::DataSnapshot{}.with_ephemeris("de440s", c.ephemerides.at("de440s"));
    auto analysis = runtime::prepare(document, data);
    const auto d = definition();
    const auto expected = runtime::generate_report(c, d);
    const auto actual = analysis.report(d);
    EXPECT_EQ(actual.table.rows(), expected.table.rows());
    EXPECT_EQ(actual.data_sha256, data.data_lock().sha256());
    EXPECT_THROW((void)analysis.report(d, [] { return true; }), runtime::AnalysisCancelled);
    auto replacement = std::make_shared<FixedSun>();
    replacement->sun = {0, 0, 149597870700.};
    replacement->info.content_sha256 = std::string(64, 'b');
    const auto changed = data.with_ephemeris("de440s", replacement);
    runtime::AnalysisPlan plan{document, changed};
    EXPECT_FALSE(plan.data_changes().empty());
    EXPECT_THROW((void)plan.prepare(), runtime::DataChangedError);
    const auto updated = plan.prepare(true).report(d);
    ASSERT_EQ(updated.table.rows().size(), 1U);
    EXPECT_EQ(std::get<std::string>(updated.table.rows()[0][5]), "Sunlight");
    EXPECT_EQ(analysis.report(d).table.rows(), actual.table.rows());
}
