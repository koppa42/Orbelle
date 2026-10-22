#include <algorithm>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <nlohmann/json.hpp>
#include <orbelle/core/propagation/hpop.hpp>
#include <orbelle/runtime/ephemeris.hpp>
#include <orbelle/runtime/gravity.hpp>
#include <orbelle/runtime/time_frames.hpp>

namespace {
using namespace orbelle;
using Clock = std::chrono::steady_clock;
using Json = nlohmann::json;
double milliseconds(Clock::time_point from) {
    return std::chrono::duration<double, std::milli>(Clock::now() - from).count();
}
class TimedFrames final : public core::IFrameProvider {
  public:
    std::shared_ptr<const core::IFrameProvider> source = runtime::make_frame_provider();
    mutable double elapsed = 0;
    mutable std::size_t positions = 0, states = 0;
    core::PositionRotation gcrf_to_itrf(core::Epoch epoch,
                                        const core::EarthOrientationSample &eop) const override {
        const auto start = Clock::now();
        auto result = source->gcrf_to_itrf(epoch, eop);
        elapsed += milliseconds(start);
        ++positions;
        return result;
    }
    core::StateTransform gcrf_to_itrf_state(core::Epoch epoch,
                                            const core::EarthOrientationState &eop) const override {
        const auto start = Clock::now();
        auto result = source->gcrf_to_itrf_state(epoch, eop);
        elapsed += milliseconds(start);
        ++states;
        return result;
    }
};
} // namespace
int main(int argc, char **argv) {
    if (argc != 4 || std::string(ORBELLE_BENCHMARK_CONFIG) != "Release") {
        std::cerr << "Usage (Release): orbelle_hpop_benchmark <data/samples> <EGM2008.gfc> "
                     "<new-output>\n";
        return 1;
    }
    try {
        const std::filesystem::path data = argv[1], output = argv[3];
        if (!std::filesystem::create_directories(output))
            throw std::runtime_error("Output directory already exists");
        Json report;
        const auto loading = Clock::now();
        const auto gravity = runtime::load_gravity(argv[2]);
        report["gravity_load_ms"] = milliseconds(loading);
        report["gravity_sha256"] = gravity->info().sha256;
        const std::shared_ptr<const core::ITimeProvider> time =
            runtime::make_builtin_time_provider();
        const std::shared_ptr<const core::IEarthOrientationProvider> eop =
            runtime::load_eop_snapshot(data / "eop/iers_c04_2016_12_28_2017_01_04/manifest.json");
        const auto ephemeris = runtime::make_third_body_ephemeris(runtime::load_spice_snapshot(
            data / "spice/de440s_2016_leap/manifest.json", output / "spice"));
        const auto first = time->from_utc({2016, 12, 31, 12, 0, 0, 0});
        const core::CartesianState initial{
            first, core::OriginId::earth, core::FrameId::gcrf, {7e6, 0, 0}, {0, 5000, 5600}};
        for (const bool forces : {false, true}) {
            for (const int hours : {1, 24}) {
                core::HpopOptions options;
                options.gravity_model = "egm2008";
                options.gravity_mu = gravity->info().mu;
                options.gravity_degree = options.gravity_order = 21;
                options.solid_tides.mode = core::SolidTideMode::permanent;
                options.drag.enabled = options.srp.enabled = forces;
                options.third_body(core::ThirdBody::sun).enabled = forces;
                options.third_body(core::ThirdBody::moon).enabled = forces;
                const core::Epoch last{first.tai_nanoseconds() + hours * 3600'000'000'000LL};
                Json runs = Json::array();
                for (int run = 0; run < 3; ++run) {
                    const auto frames = std::make_shared<TimedFrames>();
                    const auto begin = Clock::now();
                    const auto orbit = core::make_hpop_interpolated(
                        initial, first, last, options, {frames, eop, gravity, ephemeris, time});
                    const double setup = milliseconds(begin);
                    const auto start = Clock::now();
                    const auto end = orbit->state_at(last);
                    const double propagation = milliseconds(start);
                    const double frame_time = frames->elapsed;
                    const auto sample_start = Clock::now();
                    Json samples = Json::array();
                    for (int i = 0; i <= hours * 60; ++i) {
                        const auto s = orbit->state_at(
                            core::Epoch{first.tai_nanoseconds() + i * 60'000'000'000LL});
                        samples.push_back(
                            {{"position", s.position_m}, {"velocity", s.velocity_mps}});
                    }
                    const double sampling = milliseconds(sample_start);
                    runs.push_back({{"setup_ms", setup},
                                    {"propagation_ms", propagation},
                                    {"frames_ms", frame_time},
                                    {"rotation_calls", frames->positions},
                                    {"transform_calls", frames->states},
                                    {"sampling_ms", sampling},
                                    {"final_position", end.position_m},
                                    {"final_velocity", end.velocity_mps}});
                    if (run == 0)
                        report[forces ? "forces" : "default"][std::to_string(hours)]["samples"] =
                            samples;
                    std::cout << (forces ? "forces" : "default") << ' ' << hours
                              << "h: " << propagation << " ms; frames " << frame_time << " ms\n"
                              << std::flush;
                }
                report[forces ? "forces" : "default"][std::to_string(hours)]["runs"] = runs;
            }
        }
        std::ofstream file(output / "profile.json");
        file << report.dump(2) << '\n';
        if (!file)
            throw std::runtime_error("Cannot save benchmark report");
    } catch (const std::exception &error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
