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
using Json = nlohmann::json;
using Clock = std::chrono::steady_clock;
// This explicit, offline comparison adapter accepts a supplied SPK snapshot.
// Production HPOP retains its DE440s identity and GM validation in Runtime.
class ReferenceEphemeris final : public core::IThirdBodyEphemeris {
    std::shared_ptr<const core::IEphemerisProvider> source_;
    core::ThirdBodyDataInfo info_;

  public:
    explicit ReferenceEphemeris(std::shared_ptr<const core::IEphemerisProvider> source)
        : source_(std::move(source)) {
        const auto &info = source_->data_info();
        info_ = {info.model,
                 info.manifest_sha256,
                 "explicit-reference-sun-moon-v1",
                 info.first_epoch,
                 info.last_epoch,
                 1.327122e20,
                 4.902800305555e12,
                 {}};
    }
    std::array<double, 3> position(core::ThirdBody body, core::Epoch at) const override {
        if (body != core::ThirdBody::sun && body != core::ThirdBody::moon)
            throw std::invalid_argument("Reference adapter only supports Sun and Moon");
        const auto state = source_->geometric_state(core::third_body_naif_id(body), 399, at);
        if (state.frame != core::FrameId::spice_j2000 || state.epoch != at ||
            state.target_naif_id != core::third_body_naif_id(body) || state.observer_naif_id != 399)
            throw core::EphemerisDataError("Unexpected reference ephemeris convention");
        return state.position_m;
    }
    std::array<double, 3> apparent_sun_from_earth(core::Epoch at) const override {
        return source_->apparent_sun_from_earth(at);
    }
    void validate_apparent_sun_interval(core::Epoch first, core::Epoch last) const override {
        source_->validate_apparent_sun_interval(first, last);
    }
    void validate_interval(core::Epoch first, core::Epoch last,
                           std::span<const core::ThirdBody> bodies) const override {
        std::vector<std::int32_t> ids;
        for (auto body : bodies) {
            if (body != core::ThirdBody::sun && body != core::ThirdBody::moon)
                throw std::invalid_argument("Reference adapter only supports Sun and Moon");
            ids.push_back(core::third_body_naif_id(body));
        }
        source_->validate_earth_gravity_interval(first, last, ids);
    }
    const core::ThirdBodyDataInfo &data_info() const noexcept override { return info_; }
};
Json read(const std::filesystem::path &path) {
    std::ifstream input(path);
    if (!input)
        throw std::runtime_error("Cannot read " + path.string());
    return Json::parse(input);
}
void write(const std::filesystem::path &path, const Json &value) {
    std::ofstream output(path);
    output << value.dump() << '\n';
    if (!output)
        throw std::runtime_error("Cannot write " + path.string());
}
double milliseconds(Clock::time_point start) {
    return std::chrono::duration<double, std::milli>(Clock::now() - start).count();
}
core::HpopOptions options_for(const std::string &forces, double mu, const Json &settings) {
    if (forces != "point_mass" && forces != "gravity21" && forces != "tides" &&
        forces != "third_body" && forces != "drag" && forces != "srp" && forces != "full")
        throw std::invalid_argument("Unknown force configuration");
    core::HpopOptions options;
    options.gravity_model = "explicit-reference";
    options.gravity_mu = mu;
    options.gravity_degree = options.gravity_order = forces == "point_mass" ? 0 : 21;
    options.include_secular_variations = false;
    options.include_relativistic_accelerations = false;
    options.integrator = core::HpopIntegrator::rkf78;
    if (settings.value("integrator", std::string{"rkf78"}) == "rkv89")
        options.integrator = core::HpopIntegrator::rkv89;
    else if (settings.value("integrator", std::string{"rkf78"}) != "rkf78")
        throw std::invalid_argument("Unknown reference integrator");
    options.relative_tolerance = settings.value("relative_tolerance", 1e-13);
    options.position_tolerance_m = settings.value("position_tolerance_m", .001);
    options.velocity_tolerance_mps = settings.value("velocity_tolerance_mps", 1e-6);
    options.min_step_seconds = settings.value("min_step_seconds", 1.);
    options.max_step_seconds = settings.value("max_step_seconds", 86400.);
    if (settings.contains("fixed_step_seconds")) {
        options.step_control = core::HpopStepControl::fixed;
        options.fixed_step_seconds = settings.at("fixed_step_seconds");
    }
    options.minimum_altitude_m = 10000;
    options.report_ephemeris_on_fixed_step = true;
    options.interpolation.method = core::OrbitInterpolationMethod::lagrange;
    options.interpolation.order = 7;
    options.interpolation.vop_mu = mu;
    options.solid_tides.mode = forces == "full" || forces == "tides"
                                   ? core::SolidTideMode::permanent
                                   : core::SolidTideMode::none;
    options.solid_tides.truncate_to_gravity = true;
    options.solid_tides.include_time_dependent_terms = false;
    for (const auto body : {core::ThirdBody::sun, core::ThirdBody::moon}) {
        auto &value = options.third_body(body);
        value.enabled = forces == "full" || forces == "third_body";
        value.source = core::ThirdBodyGravitySource::user_specified;
        value.user_mu = body == core::ThirdBody::sun ? 1.327122e20 : 4.902800305555e12;
    }
    options.drag.enabled = forces == "full" || forces == "drag";
    options.drag.cd = 2.2;
    options.drag.area_mass_ratio_m2_per_kg = .02;
    options.drag.atmosphere = core::AtmosphereModel::jacchia_roberts;
    options.drag.f107 = options.drag.f107_average = 150;
    options.drag.kp = 3;
    options.drag.use_apparent_sun_position = true;
    options.drag.use_approximate_altitude = settings.value("use_approximate_altitude", true);
    options.srp.enabled = forces == "full" || forces == "srp";
    options.srp.cr = 1;
    options.srp.area_mass_ratio_m2_per_kg = .02;
    options.srp.shadow_model = core::SrpShadowModel::dual_cone;
    if (settings.value("shadow_model", std::string{"dual_cone"}) == "none")
        options.srp.shadow_model = core::SrpShadowModel::none;
    else if (settings.value("shadow_model", std::string{"dual_cone"}) != "dual_cone")
        throw std::invalid_argument("Unknown reference shadow model");
    options.srp.earth_shadow = options.srp.moon_shadow = true;
    options.srp.sun_position_method = core::SrpSunPositionMethod::apparent_to_true_cb;
    options.srp.boundary_mitigation = false;
    options.srp.atmospheric_altitude_m = 0;
    options.srp.pressure_at_reference_pa = settings.value("srp_pressure_pa", 4.56e-6);
    options.srp.reference_distance_m = 149597870700;
    options.srp.sun_radius_m = settings.value("sun_radius_m", 695700000.);
    options.srp.earth_radius_m = 6378137;
    options.srp.moon_radius_m = 1737400;
    core::validate_hpop_options(options);
    return options;
}
} // namespace
int main(int argc, char **argv) {
    if (argc != 7 || std::string(ORBELLE_BENCHMARK_CONFIG) != "Release") {
        std::cerr << "Usage (Release): orbelle_hpop_reference <inputs.json> <gravity> "
                     "<eop-manifest> <spk-manifest> <settings.json> <new-output>\n";
        return 1;
    }
    try {
        const auto inputs = read(argv[1]), settings = read(argv[5]);
        if (inputs.at("schema_version") != 1 || settings.at("schema_version") != 1)
            throw std::invalid_argument("Unsupported reference schema");
        const int runs = settings.value("runs", 3);
        if (runs < 1 || runs > 100)
            throw std::invalid_argument("Run count must be 1..100");
        const std::filesystem::path output = argv[6];
        if (!std::filesystem::create_directories(output))
            throw std::runtime_error("Output already exists");
        const std::shared_ptr<const core::ITimeProvider> time =
            runtime::make_builtin_time_provider();
        const auto &u = inputs.at("epoch_utc");
        const auto first = time->from_utc({u[0], u[1], u[2], u[3], u[4], u[5], 0});
        if (first.tai_nanoseconds() != inputs.at("epoch_tai_ns").get<std::int64_t>())
            throw std::invalid_argument("Reference UTC and TAI disagree");
        const std::shared_ptr<const core::IEphemerisProvider> spk =
            runtime::load_spice_snapshot(argv[4], output / "spice");
        const core::HpopEnvironment environment{
            runtime::make_frame_provider(), runtime::load_eop_snapshot(argv[3]),
            runtime::load_gravity(argv[2]), std::make_shared<ReferenceEphemeris>(spk), time};
        Json report{{"inputs", inputs},
                    {"settings", settings},
                    {"spk", read(argv[4])},
                    {"eop", read(argv[3])},
                    {"gravity_sha256", environment.gravity->info().sha256},
                    {"cases", Json::array()}};
        const auto names = settings.value("cases", std::vector<std::string>{"scenario4_week"});
        const auto forces = settings.value("forces", std::vector<std::string>{"full"});
        const auto step = inputs.at("output_step_s").get<std::int64_t>();
        if (step <= 0 || step > 86400)
            throw std::invalid_argument("Invalid report step");
        for (const auto &name : names) {
            const auto &input = inputs.at("cases").at(name);
            const auto duration = input.at("duration_s").get<std::int64_t>();
            if (duration <= 0 || duration > 366 * 86400 || duration % step)
                throw std::invalid_argument("Invalid reference duration");
            const core::Epoch last{first.tai_nanoseconds() + duration * 1'000'000'000LL};
            const core::CartesianState initial{
                first, core::OriginId::earth, core::FrameId::gcrf,
                input.at("position_m").get<std::array<double, 3>>(),
                input.at("velocity_mps").get<std::array<double, 3>>()};
            for (const auto &force : forces) {
                auto options = options_for(force, inputs.at("mu_m3_s2"), settings);
                Json row{{"name", name}, {"forces", force}, {"runs", Json::array()}};
                for (int run = 0; run < runs; ++run) {
                    const auto orbit = core::make_hpop_interpolated(
                        initial, first, last, options, environment, step * 1'000'000'000LL);
                    const auto start = Clock::now();
                    (void)orbit->state_at(last);
                    const double elapsed = milliseconds(start);
                    row["runs"].push_back({{"propagation_ms", elapsed}});
                    std::cout << name << '/' << force << " " << elapsed << " ms\n" << std::flush;
                    if (run == 0) {
                        Json samples = Json::array();
                        for (std::int64_t s = 0; s <= duration; s += step) {
                            const auto value = orbit->state_at(
                                core::Epoch{first.tai_nanoseconds() + s * 1'000'000'000LL});
                            samples.push_back({{"epoch_tai_ns", value.epoch.tai_nanoseconds()},
                                               {"position_m", value.position_m},
                                               {"velocity_mps", value.velocity_mps}});
                        }
                        write(output / (name + '-' + force + "-samples.json"), samples);
                    }
                }
                report["cases"].push_back(std::move(row));
                write(output / "profile.json", report);
            }
        }
    } catch (const std::exception &error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
