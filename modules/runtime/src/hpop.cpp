#include <orbitforge/application/project.hpp>
#include <orbitforge/runtime/ephemeris.hpp>
#include <orbitforge/runtime/gravity.hpp>
#include <orbitforge/runtime/hpop.hpp>
#include <sstream>

namespace orbitforge::runtime {
application::Session make_hpop_session(core::CartesianState initial, core::HpopOptions options,
                                       core::HpopEnvironment environment, std::string object_id,
                                       std::uint64_t revision) {
    auto algorithm =
        std::string("hpop-cowell-gcrf-tai-v1;") +
        (options.integrator == core::HpopIntegrator::rkf78 ? "rkf78-8th-v1;" : "rk4-v1;") +
        (options.gravity_model.empty() ? std::string(core::egm96_model_id())
                                       : options.gravity_model) +
        ";degree=" + std::to_string(options.gravity_degree) +
        ";order=" + std::to_string(options.gravity_order);
    if (options.gravity_degree > 0 && environment.eop)
        algorithm += ";data=" + application::eop_data_lock(environment.eop->data_info()).sha256();
    if (!options.gravity_model.empty() && environment.gravity)
        algorithm += ";gravity=" +
                     application::DataLock(
                         {gravity_reference(options.gravity_model, environment.gravity->info())})
                         .sha256();
    if (options.uses_third_bodies() && environment.ephemeris) {
        std::ostringstream config;
        config.imbue(std::locale::classic());
        config << std::hexfloat;
        for (auto body : options.enabled_third_bodies()) {
            const auto &settings = options.third_body(body);
            config << ";" << core::third_body_name(body)
                   << "-target=" << core::third_body_naif_id(body)
                   << ";source=" << static_cast<int>(settings.source) << ";mu="
                   << core::third_body_mu(settings, body, environment.ephemeris->data_info());
            if (settings.source == core::ThirdBodyGravitySource::cb_file)
                config << ";cb=" << settings.cb->content_sha256;
        }
        algorithm +=
            config.str() + ";ephemeris=" +
            application::DataLock(
                {ephemeris_reference(options.ephemeris_model, environment.ephemeris->data_info())})
                .sha256();
    }
    return {core::make_hpop(initial, options, std::move(environment)),
            {std::move(object_id), revision, algorithm}};
}
} // namespace orbitforge::runtime
