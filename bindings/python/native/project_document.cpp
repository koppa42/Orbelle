#include <orbitforge/application/project_document.hpp>
#include <pybind11/functional.h>
#include <pybind11/operators.h>
#include <pybind11/pybind11.h>
#include <pybind11/stl.h>
#include <pybind11/stl/filesystem.h>

namespace py = pybind11;
using namespace orbitforge::application;
void bind_project_document(py::module_ &module) {
    py::register_exception<ObjectNotFoundError>(module, "ObjectNotFoundError",
                                                PyExc_ReferenceError);
    py::register_exception<RevisionConflictError>(module, "RevisionConflictError",
                                                  PyExc_RuntimeError);
    py::register_exception<EditClosedError>(module, "EditClosedError", PyExc_RuntimeError);
    py::enum_<ObjectKind>(module, "ObjectKind")
        .value("SATELLITE", ObjectKind::satellite)
        .value("GROUND_STATION", ObjectKind::ground_station)
        .value("SENSOR", ObjectKind::sensor);
    py::class_<ObjectRef>(module, "ObjectRef")
        .def_property_readonly("id", &ObjectRef::id)
        .def_property_readonly("kind", &ObjectRef::kind)
        .def(py::self == py::self);
    py::class_<objects::Satellite>(module, "SatelliteDefinition")
        .def(py::init<std::string, OrbitInput, OrbitDisplay>(), py::arg("name"), py::arg("orbit"),
             py::arg("display"));
    py::class_<objects::GroundStation>(module, "GroundStationDefinition")
        .def(py::init<std::string, orbitforge::core::GroundStationLocation, double>(),
             py::arg("name"), py::arg("location"), py::arg("minimum_elevation_rad"));
    py::class_<objects::Pointing>(module, "PointingDefinition")
        .def(py::init<std::optional<ObjectRef>>(), py::arg("target") = py::none());
    py::class_<objects::Sensor>(module, "SensorDefinition")
        .def(py::init<std::string, ObjectRef, double, objects::Pointing>(), py::arg("name"),
             py::arg("parent"), py::arg("half_angle_rad"), py::arg("pointing"))
        .def(py::init<std::string, ObjectRef, orbitforge::core::SensorPattern, objects::Pointing,
                      double, SensorDisplay>(),
             py::arg("name"), py::arg("parent"), py::arg("pattern"), py::arg("pointing"),
             py::arg("roll_rad") = 0, py::arg("display") = SensorDisplay{});
    py::class_<objects::SatelliteUpdate>(module, "SatelliteUpdate")
        .def(py::init<>())
        .def_readwrite("name", &objects::SatelliteUpdate::name)
        .def_readwrite("orbit", &objects::SatelliteUpdate::orbit)
        .def_readwrite("display", &objects::SatelliteUpdate::display);
    py::class_<objects::GroundStationUpdate>(module, "GroundStationUpdate")
        .def(py::init<>())
        .def_readwrite("name", &objects::GroundStationUpdate::name)
        .def_readwrite("location", &objects::GroundStationUpdate::location)
        .def_readwrite("minimum_elevation_rad",
                       &objects::GroundStationUpdate::minimum_elevation_rad);
    py::class_<objects::SensorUpdate>(module, "SensorUpdate")
        .def(py::init<>())
        .def_readwrite("name", &objects::SensorUpdate::name)
        .def_property(
            "half_angle_rad",
            [](const objects::SensorUpdate &s) -> std::optional<double> {
                return s.pattern ? std::optional<double>(s.pattern->half_angle_rad) : std::nullopt;
            },
            [](objects::SensorUpdate &s, std::optional<double> v) {
                s.pattern = v ? std::optional<orbitforge::core::SensorPattern>(*v) : std::nullopt;
            })
        .def_readwrite("pattern", &objects::SensorUpdate::pattern)
        .def_readwrite("roll_rad", &objects::SensorUpdate::roll_rad)
        .def_readwrite("display", &objects::SensorUpdate::display)
        .def_readwrite("pointing", &objects::SensorUpdate::pointing);
    py::class_<ProjectSnapshot>(module, "ProjectSnapshot")
        .def("read", &ProjectSnapshot::read, py::arg("object"))
        .def_readonly("revision", &ProjectSnapshot::revision)
        .def_property_readonly("name", [](const ProjectSnapshot &s) { return s.project->name; })
        .def_property_readonly("first", [](const ProjectSnapshot &s) { return s.project->first; })
        .def_property_readonly("last", [](const ProjectSnapshot &s) { return s.project->last; })
        .def_property_readonly("current",
                               [](const ProjectSnapshot &s) { return s.project->current; })
        .def_property_readonly(
            "earth_orientation",
            [](const ProjectSnapshot &s) { return s.project->earth_orientation; })
        .def_property_readonly("data_lock",
                               [](const ProjectSnapshot &s) { return s.project->data_lock(); })
        .def_property_readonly("time_info",
                               [](const ProjectSnapshot &s) { return s.project->time_data_info(); })
        .def_property_readonly("analysis_record",
                               [](const ProjectSnapshot &s) { return s.project->analysis_record; })
        .def_property_readonly(
            "analysis_status",
            [](const ProjectSnapshot &s) { return s.project->analysis_status(); })
        .def_property_readonly("project", [](const ProjectSnapshot &s) { return *s.project; });
    py::class_<ObjectChange>(module, "ObjectChange")
        .def_readonly("id", &ObjectChange::id)
        .def_readonly("fields", &ObjectChange::fields);
    py::class_<ChangeSet>(module, "ChangeSet")
        .def_readonly("before_revision", &ChangeSet::before_revision)
        .def_readonly("revision", &ChangeSet::revision)
        .def_readonly("added", &ChangeSet::added)
        .def_readonly("removed", &ChangeSet::removed)
        .def_readonly("updated", &ChangeSet::updated)
        .def_readonly("settings_changed", &ChangeSet::settings_changed)
        .def_readonly("before", &ChangeSet::before)
        .def_readonly("after", &ChangeSet::after);
    py::class_<RemovalPlan>(module, "RemovalPlan")
        .def_readonly("revision", &RemovalPlan::revision)
        .def_readonly("removed", &RemovalPlan::removed)
        .def_readonly("blocking_sensors", &RemovalPlan::blocking_sensors)
        .def_readonly("affected_reports", &RemovalPlan::affected_reports);
    py::class_<ObjectManager>(module, "ObjectManager")
        .def("get", &ObjectManager::get, py::arg("id"))
        .def("find", &ObjectManager::find, py::arg("name") = py::none(),
             py::arg("kind") = py::none())
        .def("children", &ObjectManager::children, py::arg("parent"))
        .def("read", &ObjectManager::read, py::arg("ref"))
        .def("create", py::overload_cast<const objects::Satellite &>(&ObjectManager::create))
        .def("create", py::overload_cast<const objects::GroundStation &>(&ObjectManager::create))
        .def("create", py::overload_cast<const objects::Sensor &>(&ObjectManager::create))
        .def("update", py::overload_cast<const ObjectRef &, const objects::SatelliteUpdate &>(
                           &ObjectManager::update))
        .def("update", py::overload_cast<const ObjectRef &, const objects::GroundStationUpdate &>(
                           &ObjectManager::update))
        .def("update", py::overload_cast<const ObjectRef &, const objects::SensorUpdate &>(
                           &ObjectManager::update))
        .def("rename", &ObjectManager::rename, py::arg("ref"), py::arg("name"))
        .def("duplicate", &ObjectManager::duplicate, py::arg("ref"), py::arg("name"))
        .def("reparent", &ObjectManager::reparent, py::arg("sensor"), py::arg("parent"))
        .def("plan_remove", &ObjectManager::plan_remove, py::arg("ref"))
        .def("remove", &ObjectManager::remove, py::arg("ref"));
    py::class_<ProjectEdit>(module, "ProjectEdit")
        .def("objects", &ProjectEdit::objects)
        .def("snapshot", &ProjectEdit::snapshot)
        .def("settings", &ProjectEdit::settings, py::arg("name") = py::none(),
             py::arg("first") = py::none(), py::arg("last") = py::none(),
             py::arg("current") = py::none())
        .def("earth_orientation", &ProjectEdit::earth_orientation, py::arg("data"))
        .def("reports", &ProjectEdit::reports, py::arg("reports"))
        .def("commit", &ProjectEdit::commit)
        .def("discard", &ProjectEdit::discard);
    py::class_<ProjectDocument>(module, "ProjectDocument")
        .def(py::init<Project>(), py::arg("project"))
        .def("snapshot", &ProjectDocument::snapshot)
        .def("objects", &ProjectDocument::objects)
        .def("edit", &ProjectDocument::edit)
        .def_property_readonly("dirty", &ProjectDocument::dirty)
        .def("mark_saved", &ProjectDocument::mark_saved, py::arg("revision"))
        .def("save",
             py::overload_cast<const std::filesystem::path &, bool>(&ProjectDocument::save,
                                                                    py::const_),
             py::arg("path"), py::arg("overwrite") = false,
             py::call_guard<py::gil_scoped_release>())
        .def(
            "subscribe",
            [](ProjectDocument &project, py::function observer) {
                auto callback = std::shared_ptr<py::function>(new py::function(std::move(observer)),
                                                              [](py::function *function) {
                                                                  py::gil_scoped_acquire acquire;
                                                                  delete function;
                                                              });
                return project.subscribe([callback](const ChangeSet &change) {
                    py::gil_scoped_acquire acquire;
                    // Python may retain an event after the native dispatch returns.
                    (*callback)(py::cast(change, py::return_value_policy::copy));
                });
            },
            py::arg("observer"))
        .def("unsubscribe", &ProjectDocument::unsubscribe, py::arg("token"))
        .def_property_readonly("observer_errors", &ProjectDocument::observer_errors);
}
