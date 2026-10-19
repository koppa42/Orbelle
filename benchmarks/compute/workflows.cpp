#include <algorithm>
#include <bit>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <nlohmann/json.hpp>
#include <orbitforge/core/propagation/circular_two_body.hpp>
#include <orbitforge/core/propagation/tabulated_orbit.hpp>
#include <orbitforge/runtime/project.hpp>
#include <orbitforge/runtime/report.hpp>
#include <orbitforge/runtime/time_frames.hpp>
#include <tuple>

using namespace orbitforge;
namespace {
using Json = nlohmann::json;
struct Digest {
    std::uint64_t value = 14695981039346656037ULL;
    void byte(std::uint8_t v) { value = (value ^ v) * 1099511628211ULL; }
    void word(std::uint64_t v) {
        for (int i = 0; i < 8; ++i, v >>= 8)
            byte(static_cast<std::uint8_t>(v));
    }
    void real(double v) { word(std::bit_cast<std::uint64_t>(v)); }
    void state(const core::CartesianState &s) {
        word(static_cast<std::uint64_t>(s.epoch.tai_nanoseconds()));
        word(static_cast<std::uint64_t>(s.frame));
        word(static_cast<std::uint64_t>(s.origin));
        for (auto v : s.position_m)
            real(v);
        for (auto v : s.velocity_mps)
            real(v);
    }
};
template <class Operation, class Verify>
void measure(Json &output, const char *name, std::size_t operations, Operation run, Verify verify) {
    Json samples = Json::array();
    std::uint64_t digest = 0;
    for (int i = 0; i < 4; ++i) {
        const auto begin = std::chrono::steady_clock::now();
        run();
        const auto end = std::chrono::steady_clock::now();
        const auto current = verify(); // Consume complete outputs outside the timed region.
        if (i && current != digest)
            throw std::runtime_error("Non-deterministic benchmark output");
        digest = current;
        if (i)
            samples.push_back(std::chrono::duration<double, std::milli>(end - begin).count());
    }
    output["cases"][name] = {
        {"operations", operations}, {"milliseconds", samples}, {"digest", digest}};
    std::cout << name << ": " << samples.dump() << " ms\n" << std::flush;
}
application::Project large_project() {
    application::Project p{"Workflow benchmark", core::Epoch{0}, core::Epoch{1200'000'000'000}};
    for (int i = 0; i < 3000; ++i) {
        application::ProjectSatellite s;
        s.orbit.object_id = "sat-" + std::to_string(i);
        s.orbit.first = p.first;
        s.orbit.last = p.last;
        p.satellites.push_back(std::move(s));
        p.ground_stations.push_back({"station-" + std::to_string(i), "Station", {.2, .1, 0}});
    }
    for (int i = 0; i < 4000; ++i)
        p.sensors.push_back(
            {"sensor-" + std::to_string(i), "Sensor", "sat-" + std::to_string(2999 - i % 3000), .3,
             application::SensorPointing::targeted, "station-" + std::to_string(2999 - i % 3000)});
    return p;
}
std::uint64_t bytes_digest(const std::vector<std::uint8_t> &bytes) {
    Digest digest;
    for (auto b : bytes)
        digest.byte(b);
    return digest.value;
}
} // namespace
int main(int argc, char **argv) {
    try {
        if ((argc != 3 && argc != 4) || (argc == 4 && std::string(argv[3]) != "--scenario-only") ||
            std::string(ORBITFORGE_BENCHMARK_CONFIG) != "Release")
            throw std::invalid_argument(
                "Usage (Release): workflow_benchmark SAMPLE_ROOT NEW_OUTPUT");
        const std::filesystem::path samples = argv[1], output = argv[2];
        if (std::filesystem::exists(output))
            throw std::invalid_argument("Output must be a new directory");
        std::filesystem::create_directories(output);
        Json report{{"schema", 1},
                    {"configuration", ORBITFORGE_BENCHMARK_CONFIG},
                    {"warmups", 1},
                    {"repeats", 3}};
        if (argc == 4) {
            auto input = large_project();
            std::optional<application::Scenario> scene;
            measure(
                report, "assemble_6000_sessions", 6000,
                [&] { scene = runtime::make_project_scenario(input); },
                [&] {
                    Digest digest;
                    digest.word(scene->revision());
                    for (const auto &s : scene->sample(std::array{input.first, input.last})) {
                        for (auto c : s.provenance().object_id)
                            digest.byte(static_cast<std::uint8_t>(c));
                        digest.word(s.provenance().input_revision);
                        for (const auto &state : s.states())
                            digest.state(state);
                    }
                    return digest.value;
                });
            std::ofstream stream(output / "profile.json");
            stream << report.dump(2) << '\n';
            if (!stream)
                throw std::runtime_error("Cannot write scenario benchmark report");
            return 0;
        }
        auto project = large_project();
        const auto expected = application::encode_project(project);
        measure(
            report, "validate_10000_objects", 10000,
            [&] { application::validate_project(project); },
            [&] { return bytes_digest(application::encode_project(project)); });
        std::vector<std::uint8_t> encoded;
        measure(
            report, "encode_10000_objects", 10000,
            [&] { encoded = application::encode_project(project); },
            [&] {
                if (encoded != expected)
                    throw std::runtime_error("Project encoding changed");
                return bytes_digest(encoded);
            });
        auto decoded = project;
        measure(
            report, "decode_10000_objects", 10000,
            [&] { decoded = application::decode_project(expected); },
            [&] {
                const auto bytes = application::encode_project(decoded);
                if (bytes != expected)
                    throw std::runtime_error("Project round trip changed");
                return bytes_digest(bytes);
            });
        report["project_bytes"] = expected.size();

        core::CircularTwoBody orbit{core::Length{7e6}, core::GravitationalParameter{3.986004418e14},
                                    core::Epoch{0}};
        core::OrbitEphemerisArc nodes;
        for (std::int64_t i = 0; i < 1000; ++i)
            nodes.push_back(orbit.state_at(core::Epoch{i * 30'000'000'000 + i % 5 * 300'000'000}));
        for (const auto &[name, method, order, count] :
             {std::tuple{"hermite_7", core::OrbitInterpolationMethod::hermite, 7, 100000},
              std::tuple{"hermite_29", core::OrbitInterpolationMethod::hermite, 29, 100000},
              std::tuple{"lagrange_7", core::OrbitInterpolationMethod::lagrange, 7, 100000},
              std::tuple{"vop_7", core::OrbitInterpolationMethod::vop, 7, 5000}}) {
            const auto provider = core::make_tabulated_orbit({nodes}, {method, order});
            std::vector<core::CartesianState> states;
            states.reserve(static_cast<std::size_t>(count));
            measure(
                report, name, static_cast<std::size_t>(count),
                [&] {
                    states.clear();
                    for (int i = 0; i < count; ++i)
                        states.push_back(provider->state_at(core::Epoch{
                            1 + static_cast<std::int64_t>(i) * 29900'000'000'000 / count}));
                },
                [&] {
                    Digest digest;
                    for (const auto &s : states)
                        digest.state(s);
                    return digest.value;
                });
        }
        project.satellites.resize(4);
        project.ground_stations.clear();
        project.sensors.clear();
        for (std::size_t i = 0; i < project.satellites.size(); ++i) {
            auto &s = project.satellites[i];
            s.orbit.initial =
                orbit.state_at(core::Epoch{static_cast<std::int64_t>(i) * 1000'000'000'000});
            s.orbit.initial.epoch = project.first;
        }
        auto eop = runtime::load_eop_snapshot(samples /
                                              "eop/iers_c04_2000_01_01_2000_01_02/manifest.json");
        project.earth_orientation = eop->data_info();
        runtime::ReportContext context{project, runtime::make_builtin_time_provider(),
                                       runtime::make_frame_provider(), std::move(eop)};
        application::ReportDefinition definition;
        definition.id = definition.name = "Coverage benchmark";
        definition.query.kind = application::ReportKind::coverage_grid;
        definition.query.object_ids = {"sat-0", "sat-1", "sat-2", "sat-3"};
        definition.query.step_nanoseconds = 30'000'000'000;
        definition.query.event_tolerance_nanoseconds = 1'000'000;
        definition.query.coverage = {-.5, .5, -2., 2., .15};
        std::optional<runtime::ReportResult> result;
        measure(
            report, "coverage_4_assets", core::coverage_grid(definition.query.coverage).size(),
            [&] { result = runtime::generate_report(context, definition); },
            [&] {
                Digest digest;
                for (const auto &p : result->coverage_statistics->points) {
                    digest.real(p.cell.area_m2);
                    digest.real(p.statistics.covered_seconds);
                    digest.real(p.statistics.maximum_gap_seconds);
                    digest.real(p.statistics.mean_gap_seconds);
                    for (const auto &w : p.windows) {
                        digest.word(static_cast<std::uint64_t>(w.first.tai_nanoseconds()));
                        digest.word(static_cast<std::uint64_t>(w.last.tai_nanoseconds()));
                    }
                }
                return digest.value;
            });
        std::ofstream stream(output / "profile.json");
        stream << report.dump(2) << '\n';
        if (!stream)
            throw std::runtime_error("Cannot write benchmark report");
    } catch (const std::exception &error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
