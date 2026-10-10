#include <orbitforge/runtime/project.hpp>
#include <pybind11/pybind11.h>
#include <pybind11/stl.h>
#include <pybind11/stl/filesystem.h>

namespace py = pybind11;
using namespace orbitforge;
void bind_data_lock(py::module_ &module) {
    py::class_<application::DataReference>(module, "DataReference")
        .def(py::init<std::string, std::string, std::string, std::string, std::string>(),
             py::arg("role"), py::arg("snapshot_id"), py::arg("content_sha256"),
             py::arg("interpretation_version"), py::arg("configuration_sha256"))
        .def_readonly("role", &application::DataReference::role)
        .def_readonly("snapshot_id", &application::DataReference::snapshot_id)
        .def_readonly("content_sha256", &application::DataReference::content_sha256)
        .def_readonly("interpretation_version", &application::DataReference::interpretation_version)
        .def_readonly("configuration_sha256", &application::DataReference::configuration_sha256);
    py::class_<application::DataLock>(module, "DataLock")
        .def(py::init<std::vector<application::DataReference>>(),
             py::arg("references") = std::vector<application::DataReference>{})
        .def_property_readonly("sha256", &application::DataLock::sha256)
        .def_property_readonly("references",
                               [](const application::DataLock &lock) {
                                   return std::vector<application::DataReference>(
                                       lock.references().begin(), lock.references().end());
                               })
        .def("canonical_bytes", [](const application::DataLock &lock) {
            const auto bytes = lock.canonical_bytes();
            return py::bytes(reinterpret_cast<const char *>(bytes.data()), bytes.size());
        });
}
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
        .def_readwrite("display_name", &application::ProjectSatellite::display_name)
        .def_property_readonly("name", &application::ProjectSatellite::name)
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
        .def_property_readonly("data_lock", &application::Project::data_lock)
        .def("renamed_satellite", &application::Project::renamed_satellite, py::arg("object_id"),
             py::arg("name"))
        .def("without_satellite", &application::Project::without_satellite, py::arg("object_id"))
        .def("with_satellite_copy", &application::Project::with_satellite_copy,
             py::arg("object_id"), py::arg("new_id"), py::arg("name"))
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
