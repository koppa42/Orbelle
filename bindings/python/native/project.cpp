#include <orbitforge/runtime/project.hpp>
#include <pybind11/pybind11.h>
#include <pybind11/stl.h>
#include <pybind11/stl/filesystem.h>

namespace py = pybind11;
using namespace orbitforge;
void bind_project(py::module_ &module) {
    py::register_exception<application::ProjectError>(module, "ProjectError", PyExc_ValueError);
    py::enum_<application::OrbitPropagator>(module, "OrbitPropagator")
        .value("TWO_BODY", application::OrbitPropagator::two_body)
        .value("J2_SECULAR", application::OrbitPropagator::j2_secular);
    py::class_<application::OrbitInput>(module, "OrbitInput")
        .def(py::init<>())
        .def_readwrite("initial", &application::OrbitInput::initial)
        .def_readwrite("propagator", &application::OrbitInput::propagator)
        .def_readwrite("input_frame", &application::OrbitInput::input_frame)
        .def_readwrite("object_id", &application::OrbitInput::object_id)
        .def_readwrite("first", &application::OrbitInput::first)
        .def_readwrite("last", &application::OrbitInput::last)
        .def_readwrite("step_seconds", &application::OrbitInput::step_seconds);
    py::class_<application::OrbitDisplay>(module, "OrbitDisplay")
        .def(py::init<>())
        .def_readwrite("extent", &application::OrbitDisplay::extent)
        .def_readwrite("past_minutes", &application::OrbitDisplay::past_minutes)
        .def_readwrite("future_minutes", &application::OrbitDisplay::future_minutes)
        .def_readwrite("track_2d", &application::OrbitDisplay::track_2d)
        .def_readwrite("labels_2d", &application::OrbitDisplay::labels_2d)
        .def_readwrite("track_3d", &application::OrbitDisplay::track_3d);
    py::class_<application::ProjectSatellite>(module, "ProjectSatellite")
        .def(py::init<>())
        .def_readwrite("orbit", &application::ProjectSatellite::orbit)
        .def_readwrite("display", &application::ProjectSatellite::display)
        .def_readwrite("model_kind", &application::ProjectSatellite::model_kind)
        .def_readwrite("model_diameter_m", &application::ProjectSatellite::model_diameter_m)
        .def_readwrite("model_path", &application::ProjectSatellite::model_path);
    py::class_<application::Project>(module, "Project")
        .def(py::init<std::string, core::Epoch, core::Epoch>(), py::arg("name"), py::arg("first"),
             py::arg("last"))
        .def_readwrite("name", &application::Project::name)
        .def_readwrite("first", &application::Project::first)
        .def_readwrite("last", &application::Project::last)
        .def_readwrite("current", &application::Project::current)
        .def_readwrite("satellites", &application::Project::satellites)
        .def_readwrite("earth_orientation", &application::Project::earth_orientation)
        .def(
            "add_satellite",
            [](application::Project &project, application::OrbitInput input,
               application::OrbitDisplay display) {
                input.first = project.first;
                input.last = project.last;
                application::ProjectSatellite satellite;
                satellite.orbit = std::move(input);
                satellite.display = display;
                auto candidate = project;
                candidate.satellites.push_back(std::move(satellite));
                application::validate_project(candidate);
                project = std::move(candidate);
            },
            py::arg("input"), py::arg("display") = application::OrbitDisplay{})
        .def("to_scenario",
             [](const application::Project &project) {
                 auto snapshot = project;
                 py::gil_scoped_release release;
                 return runtime::make_project_scenario(snapshot);
             })
        .def(
            "save",
            [](const application::Project &project, const std::filesystem::path &path,
               bool overwrite) {
                auto snapshot = project;
                py::gil_scoped_release release;
                application::save_project(path, snapshot, overwrite);
            },
            py::arg("path"), py::arg("overwrite") = false);
    module.def("load_project", &application::load_project, py::arg("path"),
               py::call_guard<py::gil_scoped_release>());
}
