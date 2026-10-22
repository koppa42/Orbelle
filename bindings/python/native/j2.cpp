#include <orbelle/runtime/j2.hpp>
#include <orbelle/runtime/j4.hpp>
#include <orbelle/runtime/sgp4.hpp>
#include <pybind11/pybind11.h>
#include <pybind11/stl.h>

namespace py = pybind11;
using namespace orbelle;
void bind_j2(py::module_ &module) {
    py::class_<core::CentralBody>(module, "CentralBody")
        .def_readonly("origin", &core::CentralBody::origin)
        .def_property_readonly(
            "gravitational_parameter_m3_s2",
            [](const core::CentralBody &body) { return body.mu.metres_cubed_per_second_squared; })
        .def_readonly("equatorial_radius_m", &core::CentralBody::equatorial_radius_m)
        .def_readonly("j2", &core::CentralBody::j2)
        .def_readonly("j4", &core::CentralBody::j4)
        .def_property_readonly(
            "model", [](const core::CentralBody &body) { return std::string(body.model); });
    module.def("central_body", &core::central_body, py::arg("origin") = core::OriginId::earth,
               py::return_value_policy::copy);
    py::class_<core::J2MeanElements>(module, "J2MeanElements")
        .def(py::init<core::Epoch, double, double, double, double, double, double>(),
             py::arg("epoch"), py::arg("semi_major_axis_m"), py::arg("eccentricity"),
             py::arg("inclination_rad"), py::arg("raan_rad"), py::arg("argument_of_periapsis_rad"),
             py::arg("mean_anomaly_rad"))
        .def_readonly("epoch", &core::J2MeanElements::epoch)
        .def_readonly("semi_major_axis_m", &core::J2MeanElements::semi_major_axis_m)
        .def_readonly("eccentricity", &core::J2MeanElements::eccentricity)
        .def_readonly("inclination_rad", &core::J2MeanElements::inclination_rad)
        .def_readonly("raan_rad", &core::J2MeanElements::raan_rad)
        .def_readonly("argument_of_periapsis_rad", &core::J2MeanElements::argument_of_periapsis_rad)
        .def_readonly("mean_anomaly_rad", &core::J2MeanElements::mean_anomaly_rad);
    py::class_<core::J2SecularOptions>(module, "J2SecularOptions")
        .def(py::init<>())
        .def_readwrite("node_precession", &core::J2SecularOptions::node_precession)
        .def_readwrite("apsidal_precession", &core::J2SecularOptions::apsidal_precession)
        .def_readwrite("mean_motion_correction", &core::J2SecularOptions::mean_motion_correction);
    module.def("j2_mean_ellipse_state", &runtime::j2_mean_ellipse_state, py::arg("elements"),
               py::arg("body") = core::OriginId::earth, py::call_guard<py::gil_scoped_release>());
    module.def("j2_mean_ellipse_elements", &runtime::j2_mean_ellipse_elements,
               py::arg("auxiliary_state"), py::call_guard<py::gil_scoped_release>());
    module.def(
        "make_j2_session",
        [](core::J2MeanElements initial, std::string object_id, core::J2SecularOptions options,
           std::uint64_t revision, core::OriginId body) {
            return runtime::make_j2_session(initial, options, std::move(object_id), revision, body);
        },
        py::arg("initial"), py::arg("object_id"), py::arg("options") = core::J2SecularOptions{},
        py::arg("input_revision") = 1, py::arg("body") = core::OriginId::earth,
        py::call_guard<py::gil_scoped_release>());
    module.attr("J4MeanElements") = module.attr("J2MeanElements");
    module.def("j4_earth", &core::j4_earth, py::return_value_policy::copy);
    module.def("j4_mean_ellipse_state", &runtime::j4_mean_ellipse_state, py::arg("elements"),
               py::call_guard<py::gil_scoped_release>());
    module.def("j4_mean_ellipse_elements", &runtime::j4_mean_ellipse_elements,
               py::arg("auxiliary_state"), py::call_guard<py::gil_scoped_release>());
    module.def("make_j4_session", &runtime::make_j4_session, py::arg("initial"),
               py::arg("object_id"), py::arg("input_revision") = 1,
               py::call_guard<py::gil_scoped_release>());
    py::class_<core::Tle>(module, "Tle")
        .def(py::init<std::string, std::string>(), py::arg("line1"), py::arg("line2"))
        .def_readonly("line1", &core::Tle::line1)
        .def_readonly("line2", &core::Tle::line2)
        .def_property_readonly("epoch_utc",
                               [](const core::Tle &tle) { return core::parse_tle(tle).epoch_utc; })
        .def_property_readonly("catalog_id",
                               [](const core::Tle &tle) { return core::parse_tle(tle).catalog_id; })
        .def_property_readonly("period_seconds", [](const core::Tle &tle) {
            return 86400 / core::parse_tle(tle).mean_motion_revolutions_per_day;
        });
    module.def(
        "make_sgp4_session",
        [](const core::Tle &tle, const core::ITimeProvider &time, std::string object_id,
           std::uint64_t revision) {
            // OrbitInput.tle can expose a reference to a mutable parent field. Copy
            // under the GIL before another Python thread can replace those strings.
            const auto snapshot = tle;
            py::gil_scoped_release release;
            return runtime::make_sgp4_session(snapshot, time, std::move(object_id), revision);
        },
        py::arg("tle"), py::arg("time"), py::arg("object_id"), py::arg("input_revision") = 1);
}
