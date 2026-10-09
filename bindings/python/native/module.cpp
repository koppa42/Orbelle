#include <orbitforge/runtime/demo.hpp>
#include <pybind11/pybind11.h>
#include <pybind11/stl.h>

namespace py = pybind11;
using namespace orbitforge;

void bind_time_frames(py::module_ &module);
void bind_ephemeris(py::module_ &module);

PYBIND11_MODULE(_native, module) {
    module.doc() = "OrbitForge P0 native API. Time is explicit TAI, positions are metres.";
    module.attr("__version__") = ORBITFORGE_VERSION;
    py::class_<core::Epoch>(module, "Epoch")
        .def(py::init<std::int64_t>(), py::arg("tai_nanoseconds_since_2000_noon"))
        .def_property_readonly("tai_nanoseconds", &core::Epoch::tai_nanoseconds)
        .def("seconds_since", &core::Epoch::seconds_since, py::arg("other"));
    py::enum_<core::FrameId>(module, "FrameId")
        .value("GCRF", core::FrameId::gcrf)
        .value("ITRF", core::FrameId::itrf)
        .value("SPICE_J2000", core::FrameId::spice_j2000);
    py::enum_<core::OriginId>(module, "OriginId").value("EARTH", core::OriginId::earth);
    py::class_<core::CartesianState>(module, "CartesianState")
        .def(py::init<core::Epoch, core::OriginId, core::FrameId, std::array<double, 3>,
                      std::array<double, 3>>(),
             py::arg("epoch"), py::arg("origin"), py::arg("frame"), py::arg("position_m"),
             py::arg("velocity_mps"))
        .def_readonly("epoch", &core::CartesianState::epoch)
        .def_readonly("frame", &core::CartesianState::frame)
        .def_readonly("origin", &core::CartesianState::origin)
        .def_readonly("position_m", &core::CartesianState::position_m)
        .def_readonly("velocity_mps", &core::CartesianState::velocity_mps);
    py::class_<results::Provenance>(module, "Provenance")
        .def_readonly("object_id", &results::Provenance::object_id)
        .def_readonly("input_revision", &results::Provenance::input_revision)
        .def_readonly("algorithm", &results::Provenance::algorithm);
    py::class_<results::StateSeries>(module, "StateSeries")
        .def_property_readonly("provenance", &results::StateSeries::provenance)
        .def_property_readonly("states", &results::StateSeries::states);
    py::class_<application::Session>(module, "Session")
        .def(
            "sample",
            [](const application::Session &session, const std::vector<core::Epoch> &epochs) {
                return session.sample(epochs);
            },
            py::arg("epochs"), py::call_guard<py::gil_scoped_release>());
    module.def("make_circular_demo_session", &runtime::make_circular_demo_session);
    module.def("demo_epochs", &runtime::demo_epochs);
    bind_time_frames(module);
    bind_ephemeris(module);
}
