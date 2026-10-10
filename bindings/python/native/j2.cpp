#include <orbitforge/runtime/j2.hpp>
#include <pybind11/pybind11.h>
#include <pybind11/stl.h>

namespace py = pybind11;
using namespace orbitforge;
void bind_j2(py::module_ &module) {
    py::class_<core::CentralBody>(module, "CentralBody")
        .def_readonly("origin", &core::CentralBody::origin)
        .def_property_readonly(
            "gravitational_parameter_m3_s2",
            [](const core::CentralBody &body) { return body.mu.metres_cubed_per_second_squared; })
        .def_readonly("equatorial_radius_m", &core::CentralBody::equatorial_radius_m)
        .def_readonly("j2", &core::CentralBody::j2)
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
}
