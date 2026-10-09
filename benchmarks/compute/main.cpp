#include <algorithm>
#include <array>
#include <bit>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <nlohmann/json.hpp>
#include <orbitforge/application/session.hpp>
#include <orbitforge/core/propagation/circular_two_body.hpp>
#include <orbitforge/runtime/ephemeris.hpp>
#include <orbitforge/runtime/time_frames.hpp>
#include <sstream>
#include <stdexcept>
#include <string>
#include <vector>
#ifdef _WIN32
#include <windows.h>

#include <psapi.h>
#else
#include <sys/resource.h>
#include <time.h>
#endif

namespace {
using namespace orbitforge;
using Json = nlohmann::json;
using Clock = std::chrono::steady_clock;
constexpr double mu = 3.986004418e14;
constexpr std::int64_t billion = 1'000'000'000;

void require(bool condition, const std::string &message) {
    if (!condition)
        throw std::runtime_error(message);
}
std::int64_t wall_ns() {
    return std::chrono::duration_cast<std::chrono::nanoseconds>(Clock::now().time_since_epoch())
        .count();
}
std::uint64_t cpu_ns() {
#ifdef _WIN32
    FILETIME creation{}, exit{}, kernel{}, user{};
    require(GetProcessTimes(GetCurrentProcess(), &creation, &exit, &kernel, &user) != 0,
            "GetProcessTimes failed");
    const auto ticks = [](FILETIME value) {
        return (static_cast<std::uint64_t>(value.dwHighDateTime) << 32) | value.dwLowDateTime;
    };
    return (ticks(kernel) + ticks(user)) * 100;
#else
    timespec value{};
    require(clock_gettime(CLOCK_PROCESS_CPUTIME_ID, &value) == 0, "CPU clock failed");
    return static_cast<std::uint64_t>(value.tv_sec) * billion +
           static_cast<std::uint64_t>(value.tv_nsec);
#endif
}
std::uint64_t peak_rss_bytes() {
#ifdef _WIN32
    PROCESS_MEMORY_COUNTERS info{};
    require(GetProcessMemoryInfo(GetCurrentProcess(), &info, sizeof(info)) != 0,
            "GetProcessMemoryInfo failed");
    return info.PeakWorkingSetSize;
#else
    rusage info{};
    require(getrusage(RUSAGE_SELF, &info) == 0, "getrusage failed");
    return static_cast<std::uint64_t>(info.ru_maxrss) * 1024;
#endif
}
struct Digest {
    std::uint64_t value = 14695981039346656037ULL;
    void word(std::uint64_t word) {
        for (int i = 0; i < 8; ++i) {
            value = (value ^ (word & 255)) * 1099511628211ULL;
            word >>= 8;
        }
    }
    void number(double x) {
        require(std::isfinite(x), "Non-finite result");
        word(std::bit_cast<std::uint64_t>(x));
    }
    void string(const std::string &s) {
        word(s.size());
        for (const unsigned char c : s)
            value = (value ^ c) * 1099511628211ULL;
    }
    void state(const core::CartesianState &s) {
        word(static_cast<std::uint64_t>(s.epoch.tai_nanoseconds()));
        word(static_cast<std::uint64_t>(s.origin));
        word(static_cast<std::uint64_t>(s.frame));
        for (const auto x : s.position_m)
            number(x);
        for (const auto x : s.velocity_mps)
            number(x);
    }
    void state(const core::EphemerisState &s) {
        word(static_cast<std::uint64_t>(s.epoch.tai_nanoseconds()));
        word(static_cast<std::uint64_t>(s.target_naif_id));
        word(static_cast<std::uint64_t>(s.observer_naif_id));
        word(static_cast<std::uint64_t>(s.frame));
        word(static_cast<std::uint64_t>(s.velocity_time_scale));
        number(s.tdb.seconds_past_j2000);
        string(s.source_id);
        for (const auto x : s.position_m)
            number(x);
        for (const auto x : s.velocity_mps)
            number(x);
    }
    std::string hex() const {
        std::ostringstream out;
        out << std::hex << std::setw(16) << std::setfill('0') << value;
        return out.str();
    }
};
struct Profile {
    std::size_t objects, epochs, spice_epochs, switches;
    int repeats;
};
double radius(std::size_t i) { return 7e6 + static_cast<double>(i % 10) * 1e3; }
void check_metadata(const core::CartesianState &s, core::Epoch epoch, core::FrameId frame) {
    require(s.epoch == epoch && s.origin == core::OriginId::earth && s.frame == frame,
            "State metadata differs from request");
}
void check_ephemeris(const core::EphemerisState &s, const core::EphemerisState &expected) {
    require(s.epoch == expected.epoch && s.target_naif_id == expected.target_naif_id &&
                s.observer_naif_id == expected.observer_naif_id && s.frame == expected.frame &&
                s.velocity_time_scale == expected.velocity_time_scale &&
                s.tdb.seconds_past_j2000 == expected.tdb.seconds_past_j2000 &&
                s.source_id == expected.source_id && s.position_m == expected.position_m &&
                s.velocity_mps == expected.velocity_mps,
            "SPICE output differs from scalar reference");
}
template <class Prepare, class Operation, class Verify>
void measure(Json &report, const std::string &id, std::size_t operations, int repeats,
             Prepare prepare, Operation operation, Verify verify) {
    std::string digest;
    for (int iteration = 0; iteration <= repeats; ++iteration) {
        report["active_case"] = id;
        report["active_iteration"] = iteration;
        prepare();
        const auto cpu_begin = cpu_ns();
        const auto begin = wall_ns();
        operation();
        const auto end = wall_ns();
        const auto cpu_end = cpu_ns();
        auto &samples = report["samples"];
        samples.push_back({{"case", id},
                           {"phase", iteration == 0 ? "warmup" : "measure"},
                           {"iteration", iteration},
                           {"operations", operations},
                           {"begin_ns", begin},
                           {"end_ns", end},
                           {"cpu_begin_ns", cpu_begin},
                           {"cpu_end_ns", cpu_end},
                           {"valid", false}});
        require(end > begin && cpu_end >= cpu_begin, "Invalid clock interval");
        auto checks = verify();
        const auto current = checks.at("digest").template get<std::string>();
        require(iteration == 0 || digest == current, "Non-deterministic result between iterations");
        digest = current;
        samples.back()["checks"] = std::move(checks);
        samples.back()["process_lifetime_peak_rss_bytes"] = peak_rss_bytes();
        samples.back()["valid"] = true;
    }
    std::cout << id << ": " << operations << " operations, " << repeats << " measured rounds\n"
              << std::flush;
}

void run(Json &report, const std::filesystem::path &data, const std::filesystem::path &workspace,
         const Profile &p) {
    std::vector<core::Epoch> epochs;
    std::vector<application::Session> sessions;
    for (std::size_t i = 0; i < p.epochs; ++i)
        epochs.emplace_back(static_cast<std::int64_t>(i) * billion);
    for (std::size_t i = 0; i < p.objects; ++i)
        sessions.emplace_back(
            std::make_shared<core::CircularTwoBody>(
                core::Length{radius(i)}, core::GravitationalParameter{mu},
                core::Epoch{-static_cast<std::int64_t>(i) * billion}),
            results::Provenance{"compute-" + std::to_string(i), 1, "p0-circular-two-body-v1"});
    const auto setup_begin = wall_ns();
    auto eop =
        runtime::load_eop_snapshot(data / "eop/iers_c04_2000_01_01_2000_01_02/manifest.json");
    auto frames = runtime::make_frame_provider();
    auto spice =
        runtime::load_spice_snapshot(data / "spice/de440s_2016_leap/manifest.json", workspace);
    auto other =
        runtime::load_spice_snapshot(data / "spice/de440s_2016_leap/manifest.json", workspace);
    const auto setup_end = wall_ns();
    const auto &ed = eop->data_info();
    const auto &sd = spice->data_info();
    const auto eop_source = eop->at(epochs.front()).sample.source_id;
    const auto spice_source = spice->geometric_state(10, 399, sd.first_epoch).source_id;
    require(eop_source.find(ed.sha256) != std::string::npos &&
                eop_source.find(ed.time_data.sha256) != std::string::npos &&
                spice_source.find(sd.manifest_sha256) != std::string::npos &&
                spice_source.find(sd.toolkit_version) != std::string::npos,
            "Missing data hashes in provenance");
    require(ed.sha256 == "7c49148548609953431d1c6063d08d331f869edfc057cdd74d9a465a94c1f261" &&
                ed.id == "iers-c04-p0-tai-zero-v1",
            "Unexpected EOP snapshot");
    require(sd.manifest_sha256 ==
                    "2b1a07664f8a2d52ef3d22b4f58023561d47a64f21f7ee544dfa421f7be90ce4" &&
                other->data_info().manifest_sha256 == sd.manifest_sha256,
            "Unexpected SPICE snapshot");
    report["data"] = {{"eop_id", ed.id},
                      {"eop_sha256", ed.sha256},
                      {"eop_interpolation", ed.interpolation},
                      {"eop_reference_frame", ed.reference_frame},
                      {"time_id", ed.time_data.id},
                      {"time_sha256", ed.time_data.sha256},
                      {"spice_id", sd.id},
                      {"spice_manifest_sha256", sd.manifest_sha256},
                      {"spice_model", sd.model},
                      {"spice_toolkit_version", sd.toolkit_version},
                      {"spice_first_tai_ns", sd.first_epoch.tai_nanoseconds()},
                      {"spice_last_tai_ns", sd.last_epoch.tai_nanoseconds()},
                      {"provider_setup_ns", setup_end - setup_begin}};
    for (const auto &kernel : sd.kernels)
        report["data"]["kernels"].push_back({{"file", kernel.file}, {"sha256", kernel.sha256}});
    report["data"]["eop_source_id"] = eop_source;
    report["data"]["spice_source_id"] = spice_source;

    std::vector<results::StateSeries> states;
    measure(
        report, "application_sample", p.objects * p.epochs, p.repeats,
        [&] {
            states.clear();
            states.shrink_to_fit();
        },
        [&] {
            states.reserve(p.objects);
            for (const auto &session : sessions)
                states.push_back(session.sample(epochs));
        },
        [&] {
            Digest digest;
            double dr = 0, de = 0, dh = 0, phase_p = 0, phase_v = 0;
            require(states.size() == p.objects, "Incorrect number of series");
            for (std::size_t i = 0; i < states.size(); ++i) {
                const auto &series = states[i];
                const auto &provenance = series.provenance();
                require(provenance.object_id == "compute-" + std::to_string(i) &&
                            provenance.input_revision == 1 &&
                            provenance.algorithm == "p0-circular-two-body-v1",
                        "Incorrect provenance");
                require(series.states().size() == p.epochs, "Incorrect number of states");
                digest.string(provenance.object_id);
                digest.word(provenance.input_revision);
                digest.string(provenance.algorithm);
                for (std::size_t j = 0; j < p.epochs; ++j) {
                    const auto &s = series.states()[j];
                    check_metadata(s, epochs[j], core::FrameId::gcrf);
                    digest.state(s);
                    const auto &r = s.position_m;
                    const auto &v = s.velocity_mps;
                    const auto length = std::hypot(r[0], r[1], r[2]);
                    const auto energy = (v[0] * v[0] + v[1] * v[1] + v[2] * v[2]) / 2 - mu / length;
                    dr = std::max(dr, std::abs(length - radius(i)));
                    de = std::max(de, std::abs(energy + mu / (2 * radius(i))));
                    dh = std::max(dh,
                                  std::abs(r[0] * v[1] - r[1] * v[0] - std::sqrt(mu * radius(i))));
                    const auto distance = radius(i);
                    const auto phase = std::sqrt(mu / (distance * distance * distance)) *
                                       static_cast<double>(i + j);
                    const auto speed = std::sqrt(mu / distance);
                    phase_p = std::max({phase_p, std::abs(r[0] - distance * std::cos(phase)),
                                        std::abs(r[1] - distance * std::sin(phase))});
                    phase_v = std::max({phase_v, std::abs(v[0] + speed * std::sin(phase)),
                                        std::abs(v[1] - speed * std::cos(phase))});
                    require(r[2] == 0 && v[2] == 0, "Fixture left the XY plane");
                }
            }
            require(dr <= 1e-8 && de <= 1e-7 && dh <= 1e-4, "Circular invariants exceed tolerance");
            require(phase_p <= 1e-7 && phase_v <= 1e-9,
                    "Circular phase differs from fixed workload");
            return Json{{"digest", digest.hex()},          {"max_radius_error_m", dr},
                        {"max_energy_error_m2_s2", de},    {"max_momentum_error_m2_s", dh},
                        {"max_phase_position_m", phase_p}, {"max_phase_velocity_mps", phase_v}};
        });

    std::vector<core::StateTransform> transforms;
    measure(
        report, "frame_build", p.epochs, p.repeats,
        [&] {
            transforms.clear();
            transforms.shrink_to_fit();
        },
        [&] {
            transforms.reserve(p.epochs);
            for (const auto epoch : epochs)
                transforms.push_back(frames->gcrf_to_itrf_state(epoch, eop->at(epoch)));
        },
        [&] {
            Digest digest;
            require(transforms.size() == p.epochs, "Incorrect transform count");
            for (std::size_t j = 0; j < p.epochs; ++j) {
                const auto &t = transforms[j];
                require(t.rotation.epoch == epochs[j] &&
                            t.rotation.origin == core::OriginId::earth &&
                            t.rotation.from == core::FrameId::gcrf &&
                            t.rotation.to == core::FrameId::itrf &&
                            t.rotation.eop_source_id == eop_source && !t.rotation.model.empty() &&
                            !t.derivative_model.empty(),
                        "Incorrect transform metadata");
                digest.word(static_cast<std::uint64_t>(t.rotation.epoch.tai_nanoseconds()));
                digest.string(t.rotation.model);
                digest.string(t.derivative_model);
                digest.string(t.rotation.eop_source_id);
                for (const auto &row : t.rotation.matrix)
                    for (const auto x : row)
                        digest.number(x);
                for (const auto &row : t.rotation_rate_per_second)
                    for (const auto x : row)
                        digest.number(x);
            }
            return Json{{"digest", digest.hex()}};
        });
    report["data"]["frame_model"] = transforms.front().rotation.model;
    report["data"]["frame_derivative_model"] = transforms.front().derivative_model;

    std::vector<core::CartesianState> transformed;
    measure(
        report, "frame_apply", p.objects * p.epochs, p.repeats,
        [&] {
            transformed.clear();
            transformed.shrink_to_fit();
        },
        [&] {
            transformed.reserve(p.objects * p.epochs);
            for (const auto &series : states)
                for (std::size_t j = 0; j < p.epochs; ++j)
                    transformed.push_back(transforms[j].apply(series.states()[j]));
        },
        [&] {
            Digest digest;
            double dp = 0, dv = 0;
            require(transformed.size() == p.objects * p.epochs, "Incorrect transformed count");
            for (std::size_t i = 0; i < p.objects; ++i)
                for (std::size_t j = 0; j < p.epochs; ++j) {
                    const auto &s = transformed[i * p.epochs + j];
                    check_metadata(s, epochs[j], core::FrameId::itrf);
                    digest.state(s);
                    const auto back = transforms[j].inverse(s);
                    check_metadata(back, epochs[j], core::FrameId::gcrf);
                    const auto &initial = states[i].states()[j];
                    for (std::size_t k = 0; k < 3; ++k) {
                        require(std::isfinite(back.position_m[k]) &&
                                    std::isfinite(back.velocity_mps[k]),
                                "Non-finite inverse state");
                        dp = std::max(dp, std::abs(back.position_m[k] - initial.position_m[k]));
                        dv = std::max(dv, std::abs(back.velocity_mps[k] - initial.velocity_mps[k]));
                    }
                }
            require(dp <= 5e-8 && dv <= 5e-10, "Frame round trip exceeds tolerance");
            return Json{{"digest", digest.hex()},
                        {"max_roundtrip_position_m", dp},
                        {"max_roundtrip_velocity_mps", dv}};
        });
    states.clear();
    states.shrink_to_fit();
    transformed.clear();
    transformed.shrink_to_fit();

    std::vector<core::Epoch> spice_epochs;
    for (std::size_t i = 0; i < p.spice_epochs; ++i)
        spice_epochs.emplace_back(sd.first_epoch.tai_nanoseconds() +
                                  static_cast<std::int64_t>(i) * 60 * billion);
    const std::array<std::int32_t, 2> targets{10, 301};
    using Ephemerides = std::array<std::vector<core::EphemerisState>, 2>;
    Ephemerides reference, output;
    const auto clear = [&] {
        for (auto &list : output) {
            list.clear();
            list.shrink_to_fit();
        }
    };
    const auto verify = [&] {
        Digest digest;
        for (std::size_t body = 0; body < targets.size(); ++body) {
            require(output[body].size() == p.spice_epochs, "Incorrect ephemeris count");
            for (std::size_t i = 0; i < p.spice_epochs; ++i) {
                const auto &s = output[body][i];
                require(s.epoch == spice_epochs[i] && s.target_naif_id == targets[body] &&
                            s.observer_naif_id == 399 && s.frame == core::FrameId::spice_j2000 &&
                            s.velocity_time_scale == core::TimeScale::tdb &&
                            s.source_id == spice_source,
                        "Incorrect ephemeris metadata");
                digest.state(s);
                if (!reference[body].empty())
                    check_ephemeris(s, reference[body][i]);
            }
        }
        if (reference[0].empty())
            reference = output;
        return Json{{"digest", digest.hex()}};
    };
    measure(
        report, "spice_scalar", 2 * p.spice_epochs, p.repeats, clear,
        [&] {
            for (std::size_t body = 0; body < targets.size(); ++body) {
                output[body].reserve(p.spice_epochs);
                for (const auto epoch : spice_epochs)
                    output[body].push_back(spice->geometric_state(targets[body], 399, epoch));
            }
        },
        verify);
    measure(
        report, "spice_batch", 2 * p.spice_epochs, p.repeats, clear,
        [&] {
            for (std::size_t body = 0; body < targets.size(); ++body)
                output[body] = spice->geometric_states(targets[body], 399, spice_epochs);
        },
        verify);
    std::vector<core::EphemerisState> switched;
    measure(
        report, "spice_context_switch", p.switches, p.repeats,
        [&] {
            switched.clear();
            switched.shrink_to_fit();
            (void)other->geometric_state(301, 399, spice_epochs.front());
        },
        [&] {
            switched.reserve(p.switches);
            for (std::size_t i = 0; i < p.switches; ++i) {
                const auto &provider = i % 2 == 0 ? spice : other;
                switched.push_back(
                    provider->geometric_state(targets[i % 2], 399, spice_epochs[i / 2]));
            }
        },
        [&] {
            Digest digest;
            require(switched.size() == p.switches, "Incorrect context-switch count");
            for (std::size_t i = 0; i < p.switches; ++i) {
                check_ephemeris(switched[i], reference[i % 2][i / 2]);
                digest.state(switched[i]);
            }
            return Json{{"digest", digest.hex()}};
        });
}
} // namespace

int main(int argc, char **argv) {
    Json report{
        {"schema", 1}, {"protocol", "p0-compute-v1"}, {"valid", false}, {"samples", Json::array()}};
    std::filesystem::path output;
    int result = 1;
    try {
        require(argc == 9,
                "Usage: --profile baseline|smoke --data-root PATH --workspace PATH --output PATH");
        Json args = Json::object();
        for (int i = 1; i < argc; i += 2) {
            const std::string key{argv[i]};
            require(key == "--profile" || key == "--data-root" || key == "--workspace" ||
                        key == "--output",
                    "Unknown argument: " + key);
            require(!args.contains(key), "Duplicate argument: " + key);
            args[key] = argv[i + 1];
        }
        output = args.at("--output").get<std::string>();
        require(!std::filesystem::exists(output), "Output already exists");
        const auto profile = args.at("--profile").get<std::string>();
        require(profile == "baseline" || profile == "smoke", "Unknown profile");
        const bool eligible = std::string{ORBITFORGE_BENCHMARK_CONFIG} == "Release" &&
                              ORBITFORGE_BENCHMARK_SANITIZERS == 0;
        require(profile != "baseline" || eligible, "Baseline requires Release without sanitizers");
        const Profile p =
            profile == "baseline" ? Profile{10'000, 601, 10'000, 200, 5} : Profile{8, 7, 16, 8, 2};
        report["profile"] = profile;
        report["build"] = {{"configuration", ORBITFORGE_BENCHMARK_CONFIG},
                           {"compiler", ORBITFORGE_BENCHMARK_COMPILER},
                           {"sanitizers", ORBITFORGE_BENCHMARK_SANITIZERS != 0}};
        report["workload"] = {{"objects", p.objects},
                              {"epochs", p.epochs},
                              {"spice_epochs_per_body", p.spice_epochs},
                              {"context_switches", p.switches},
                              {"warmup_rounds", 1},
                              {"measured_rounds", p.repeats}};
        report["clocks"] = {{"wall", "std::chrono::steady_clock"},
                            {"steady", Clock::is_steady},
                            {"wall_period_num", Clock::period::num},
                            {"wall_period_den", Clock::period::den},
                            {"unit", "ns"}};
#ifdef _WIN32
        report["clocks"]["cpu"] =
            "GetProcessTimes (100 ns representation; scheduler accounting granularity)";
#else
        timespec resolution{};
        require(clock_getres(CLOCK_PROCESS_CPUTIME_ID, &resolution) == 0,
                "CPU resolution unavailable");
        report["clocks"]["cpu"] = "CLOCK_PROCESS_CPUTIME_ID";
        report["clocks"]["cpu_resolution_ns"] = resolution.tv_sec * billion + resolution.tv_nsec;
#endif
        require(Clock::is_steady, "A monotonic clock is required");
        run(report, args.at("--data-root").get<std::string>(),
            args.at("--workspace").get<std::string>(), p);
        report.erase("active_case");
        report.erase("active_iteration");
        report["valid"] = true;
        result = 0;
    } catch (const std::exception &error) {
        report["error"] = error.what();
        std::cerr << error.what() << '\n';
    }
    if (!output.empty() && !std::filesystem::exists(output)) {
        std::ofstream stream(output);
        stream << report.dump(2) << '\n';
        if (!stream) {
            std::cerr << "Could not write report\n";
            return 1;
        }
    }
    return result;
}
