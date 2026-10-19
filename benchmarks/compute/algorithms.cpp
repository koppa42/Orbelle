#include <algorithm>
#include <bit>
#include <chrono>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <nlohmann/json.hpp>
#include <orbitforge/core/propagation/access.hpp>
#include <orbitforge/core/propagation/gravity.hpp>
#include <orbitforge/core/propagation/hpop.hpp>
#include <orbitforge/runtime/report.hpp>
#include <orbitforge/runtime/time_frames.hpp>

namespace {
using namespace orbitforge;
using Json = nlohmann::json;
using Clock = std::chrono::steady_clock;
struct Digest {
    std::uint64_t value = 14695981039346656037ULL;
    void word(std::uint64_t v) {
        for (int i = 0; i < 8; ++i) {
            value = (value ^ (v & 255)) * 1099511628211ULL;
            v >>= 8;
        }
    }
    void number(double v) {
        if (!std::isfinite(v))
            throw std::runtime_error("Nonfinite benchmark output");
        word(std::bit_cast<std::uint64_t>(v));
    }
};
template <class Operation, class Verify>
void measure(Json &report, const char *name, Operation operation, Verify verify) {
    std::vector<double> times;
    std::uint64_t digest = 0;
    for (int iteration = 0; iteration < 6; ++iteration) {
        const auto begin = Clock::now();
        const auto output = operation();
        const auto end = Clock::now();
        const auto current = verify(output);
        if (iteration && current != digest)
            throw std::runtime_error("Non-deterministic benchmark output");
        digest = current;
        if (iteration)
            times.push_back(std::chrono::duration<double, std::milli>(end - begin).count());
    }
    auto sorted = times;
    std::sort(sorted.begin(), sorted.end());
    report[name] = {{"samples_ms", times}, {"median_ms", sorted[2]}, {"digest", digest}};
    std::cout << name << ": " << sorted[2] << " ms\n" << std::flush;
}
} // namespace

int main(int argc, char **argv) {
    if (argc != 3 || std::string(ORBITFORGE_BENCHMARK_CONFIG) != "Release") {
        std::cerr << "Usage (Release): orbitforge_algorithm_benchmark <data-root> <new-output>\n";
        return 1;
    }
    const std::filesystem::path data = argv[1], output = argv[2];
    if (std::filesystem::exists(output))
        return 1;
    std::filesystem::create_directories(output);
    try {
        Json report;
        const auto gravity = core::builtin_egm96();
        std::vector<std::array<double, 3>> positions;
        for (int i = 0; i < 10000; ++i) {
            const double a = i * .001;
            positions.push_back({7e6 * std::cos(a), 4e6 * std::sin(a), 5e6 * std::sin(a)});
        }
        measure(
            report, "gravity_20_acceleration",
            [&] {
                std::vector<std::array<double, 3>> values;
                values.reserve(positions.size());
                for (const auto &p : positions)
                    values.push_back(gravity->acceleration(p, 20, 20));
                return values;
            },
            [](const auto &values) {
                Digest digest;
                for (const auto &v : values)
                    for (double x : v)
                        digest.number(x);
                return digest.value;
            });
        measure(
            report, "gravity_20_differential",
            [&] {
                std::vector<core::GravityDifferential> values;
                values.reserve(positions.size());
                for (const auto &p : positions)
                    values.push_back(gravity->differential(p, 20, 20));
                return values;
            },
            [](const auto &values) {
                Digest digest;
                for (const auto &v : values) {
                    for (double x : v.acceleration_mps2)
                        digest.number(x);
                    for (const auto &row : v.position_jacobian_per_s2)
                        for (double x : row)
                            digest.number(x);
                }
                return digest.value;
            });
        measure(
            report, "constraint_intervals",
            [] {
                return core::find_constraint_intervals(
                    core::Epoch{0}, core::Epoch{86400'000'000'000}, 30'000'000'000,
                    [](core::Epoch t) {
                        return std::sin(t.seconds_since(core::Epoch{0}) * .013) - .2;
                    },
                    1'000'000);
            },
            [](const auto &windows) {
                Digest digest;
                digest.word(windows.size());
                for (const auto &w : windows) {
                    digest.word(static_cast<std::uint64_t>(w.first.tai_nanoseconds()));
                    digest.word(static_cast<std::uint64_t>(w.last.tai_nanoseconds()));
                    digest.word(w.start_clipped);
                    digest.word(w.stop_clipped);
                }
                return digest.value;
            });
        runtime::ReportContext context{
            application::Project{"Algorithm benchmark", core::Epoch{0},
                                 core::Epoch{600'000'000'000}},
            runtime::make_builtin_time_provider(), runtime::make_frame_provider(),
            runtime::load_eop_snapshot(data / "eop/iers_c04_2000_01_01_2000_01_02/manifest.json")};
        context.project.earth_orientation = context.eop->data_info();
        for (int i = 0; i < 32; ++i) {
            application::ProjectSatellite satellite;
            satellite.orbit.object_id = "satellite-" + std::to_string(i);
            satellite.orbit.first = context.project.first;
            satellite.orbit.last = context.project.last;
            context.project.satellites.push_back(satellite);
        }
        application::ReportDefinition definition;
        definition.id = definition.name = "ITRF benchmark";
        definition.query.scope = application::ReportScope::all_compatible;
        definition.query.frame = core::FrameId::itrf;
        definition.query.step_nanoseconds = 1'000'000'000;
        definition.format.time_scale = application::ReportTimeScale::tai;
        measure(
            report, "report_32_objects_itrf",
            [&] { return runtime::generate_report(context, definition); },
            [](const auto &result) {
                if (result.table.rows().size() != 32 * 601)
                    throw std::runtime_error("Wrong report row count");
                Digest digest;
                for (const auto &row : result.table.rows()) {
                    digest.word(static_cast<std::uint64_t>(
                        std::get<core::Epoch>(row[2]).tai_nanoseconds()));
                    for (std::size_t i = 4; i < 10; ++i)
                        digest.number(std::get<double>(row[i]));
                }
                return digest.value;
            });
        measure(
            report, "hpop_20_one_hour",
            [&] {
                core::HpopOptions options;
                options.gravity_degree = options.gravity_order = 20;
                const core::CartesianState initial{core::Epoch{0},
                                                   core::OriginId::earth,
                                                   core::FrameId::gcrf,
                                                   {7e6, 0, 0},
                                                   {0, 5000, 5600}};
                const auto orbit = core::make_hpop(
                    initial, options, core::HpopEnvironment{context.frames, context.eop, gravity});
                return orbit->state_at(core::Epoch{3600'000'000'000});
            },
            [](const auto &state) {
                Digest digest;
                for (double v : state.position_m)
                    digest.number(v);
                for (double v : state.velocity_mps)
                    digest.number(v);
                return digest.value;
            });
        std::ofstream file(output / "profile.json");
        file << report.dump(2) << '\n';
        if (!file)
            throw std::runtime_error("Could not save benchmark report");
    } catch (const std::exception &error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
