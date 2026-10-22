#include <orbelle/core/propagation/orbit_design.hpp>
#include <pybind11/pybind11.h>
namespace py = pybind11;
using namespace orbelle;
void bind_orbit_design(py::module_ &module) {
    py::enum_<core::EarthOrbitType>(module, "EarthOrbitType")
        .value("CIRCULAR", core::EarthOrbitType::circular)
        .value("CRITICALLY_INCLINED", core::EarthOrbitType::critically_inclined)
        .value("CRITICALLY_INCLINED_SUN_SYNC", core::EarthOrbitType::critically_inclined_sun_sync)
        .value("GEOSYNCHRONOUS", core::EarthOrbitType::geosynchronous)
        .value("MOLNIYA", core::EarthOrbitType::molniya)
        .value("ORBIT_DESIGNER", core::EarthOrbitType::orbit_designer)
        .value("REPEATING_GROUND_TRACE", core::EarthOrbitType::repeating_ground_trace)
        .value("REPEATING_SUN_SYNC", core::EarthOrbitType::repeating_sun_sync)
        .value("SUN_SYNCHRONOUS", core::EarthOrbitType::sun_synchronous);
    py::class_<core::EarthOrbitDesignRequest>(module, "EarthOrbitDesignRequest")
        .def(py::init<>())
        .def_readwrite("type", &core::EarthOrbitDesignRequest::type)
        .def_readwrite("epoch", &core::EarthOrbitDesignRequest::epoch)
        .def_readwrite("altitude_m", &core::EarthOrbitDesignRequest::altitude_m)
        .def_readwrite("perigee_altitude_m", &core::EarthOrbitDesignRequest::perigee_altitude_m)
        .def_readwrite("apogee_altitude_m", &core::EarthOrbitDesignRequest::apogee_altitude_m)
        .def_readwrite("semi_major_axis_m", &core::EarthOrbitDesignRequest::semi_major_axis_m)
        .def_readwrite("eccentricity", &core::EarthOrbitDesignRequest::eccentricity)
        .def_readwrite("inclination_rad", &core::EarthOrbitDesignRequest::inclination_rad)
        .def_readwrite("raan_rad", &core::EarthOrbitDesignRequest::raan_rad)
        .def_readwrite("argument_of_periapsis_rad",
                       &core::EarthOrbitDesignRequest::argument_of_periapsis_rad)
        .def_readwrite("true_anomaly_rad", &core::EarthOrbitDesignRequest::true_anomaly_rad)
        .def_readwrite("retrograde", &core::EarthOrbitDesignRequest::retrograde)
        .def_readwrite("sun_sync_by_inclination",
                       &core::EarthOrbitDesignRequest::sun_sync_by_inclination)
        .def_readwrite("repeat_revolutions", &core::EarthOrbitDesignRequest::repeat_revolutions)
        .def_readwrite("repeat_days", &core::EarthOrbitDesignRequest::repeat_days);
    py::class_<core::EarthOrbitDesign>(module, "EarthOrbitDesign")
        .def_readonly("elements", &core::EarthOrbitDesign::elements)
        .def_readonly("secular_j2", &core::EarthOrbitDesign::secular_j2)
        .def_readonly("nodal_period_seconds", &core::EarthOrbitDesign::nodal_period_seconds)
        .def_readonly("repeat_cycle_seconds", &core::EarthOrbitDesign::repeat_cycle_seconds)
        .def_readonly("raan_rate_rad_s", &core::EarthOrbitDesign::raan_rate_rad_s)
        .def_readonly("periapsis_rate_rad_s", &core::EarthOrbitDesign::periapsis_rate_rad_s);
    module.def(
        "design_earth_orbit",
        [](const core::EarthOrbitDesignRequest &request) {
            const auto snapshot = request;
            py::gil_scoped_release release;
            return core::design_earth_orbit(snapshot);
        },
        py::arg("request"));
}
