#include <orbitforge/application/project.hpp>
#include <orbitforge/runtime/ephemeris.hpp>
#include <orbitforge/runtime/gravity.hpp>
#include <orbitforge/runtime/hpop.hpp>
#include <orbitforge/runtime/space_weather.hpp>
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
    if ((options.gravity_degree > 0 || options.drag.enabled) && environment.eop)
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
        algorithm += config.str();
    }
    if (options.srp.enabled) {
        const auto o = core::effective_srp_options(options.srp);
        std::ostringstream config;
        config.imbue(std::locale::classic());
        config << std::hexfloat << ";srp=spherical-uniform-angular-union-v2"
               << ";sun-method=" << static_cast<int>(o.sun_position_method) << ";sun-correction="
               << (o.sun_position_method == core::SrpSunPositionMethod::geometric ? "NONE"
                   : o.sun_position_method == core::SrpSunPositionMethod::apparent_to_true_cb
                       ? "earth-LT+S-v1"
                       : "spacecraft-LT+S-tai-rate-v1")
               << ";occultors=geometric"
               << ";shadow=" << static_cast<int>(o.shadow_model)
               << ";earth-shadow=" << o.earth_shadow << ";moon-shadow=" << o.moon_shadow
               << ";cr=" << o.cr << ";area-mass=" << o.area_mass_ratio_m2_per_kg
               << ";atmosphere-height=" << o.atmospheric_altitude_m
               << ";pressure=" << o.pressure_at_reference_pa
               << ";reference-distance=" << o.reference_distance_m
               << ";sun-radius=" << o.sun_radius_m << ";earth-radius=" << o.earth_radius_m
               << ";moon-radius=" << o.moon_radius_m;
        algorithm += config.str();
    }
    if (options.drag.enabled) {
        const auto d = core::effective_drag_options(options.drag);
        std::ostringstream config;
        config.imbue(std::locale::classic());
        config << std::hexfloat << ";drag=spherical-corotating-itrf-wgs84-v2"
               << ";density-altitude="
               << (d.use_approximate_altitude ? "radial-wgs84-v1" : "geodetic-wgs84-v1")
               << ";atmosphere=" << static_cast<int>(d.atmosphere)
               << ";density-kernel=jr-gmat-c9b78bd;nrlmsise00-orekit-13.1.2-gtd7d"
               << ";cd=" << d.cd << ";drag-area-mass=" << d.area_mass_ratio_m2_per_kg
               << ";low-altitude=" << static_cast<int>(d.low_altitude_model)
               << ";blend=" << d.blending_range_m
               << ";sun=" << (d.use_apparent_sun_position ? "earth-LT+S" : "NONE");
        if (d.weather_model.empty())
            config << ";weather=manual-daily;f107=" << d.f107 << ";f107-average=" << d.f107_average
                   << ";kp=" << d.kp;
        algorithm += config.str();
        if (!d.weather_model.empty() && environment.weather) {
            algorithm +=
                ";weather=file;boundary=utc-one-sided-v1;update=" +
                std::to_string(static_cast<int>(d.geomagnetic_update)) +
                ";source=" + std::to_string(static_cast<int>(d.geomagnetic_source)) +
                ";weather-data=" +
                application::DataLock(
                    {space_weather_reference(d.weather_model, environment.weather->info())})
                    .sha256();
        }
    }
    if (options.uses_ephemeris() && environment.ephemeris) {
        algorithm +=
            ";ephemeris=" +
            application::DataLock(
                {ephemeris_reference(options.ephemeris_model, environment.ephemeris->data_info())})
                .sha256();
    }
    return {core::make_hpop(initial, options, std::move(environment)),
            {std::move(object_id), revision, algorithm}};
}
} // namespace orbitforge::runtime
