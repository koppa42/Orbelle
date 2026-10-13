#include <orbitforge/runtime/analysis.hpp>
#include <orbitforge/runtime/data_catalog.hpp>
#include <pybind11/pybind11.h>
#include <pybind11/stl.h>
#include <pybind11/stl/filesystem.h>

namespace py = pybind11;
using namespace orbitforge;
void bind_analysis(py::module_ &module) {
    module.attr("MAX_ANALYSIS_SAMPLES") = runtime::max_analysis_samples;
    py::register_exception<runtime::AnalysisCapabilityError>(module, "AnalysisCapabilityError",
                                                             PyExc_RuntimeError);
    auto data_changed = py::register_exception<runtime::DataChangedError>(
        module, "DataChangedError", PyExc_RuntimeError);
    py::register_exception<runtime::OpenDataChangedError>(module, "OpenDataChangedError",
                                                          data_changed.ptr());
    module.def("save_analysis_project", &runtime::save_project, py::arg("project"), py::arg("path"),
               py::arg("analysis"), py::arg("overwrite") = false,
               py::call_guard<py::gil_scoped_release>());
    py::class_<runtime::DataSnapshot>(module, "DataSnapshot")
        .def(py::init<>())
        .def_property_readonly("selection_notes", &runtime::DataSnapshot::selection_notes)
        .def_static("builtin_time", &runtime::DataSnapshot::builtin_time,
                    py::call_guard<py::gil_scoped_release>())
        .def_static("load", &runtime::DataSnapshot::load, py::kw_only(),
                    py::arg("eop_manifest") = "", py::arg("time_file") = "",
                    py::arg("extrapolation_days") = 0, py::call_guard<py::gil_scoped_release>())
        .def_property_readonly("data_lock", &runtime::DataSnapshot::data_lock,
                               py::return_value_policy::copy)
        .def_property_readonly(
            "time_info",
            [](const runtime::DataSnapshot &data) -> std::optional<core::TimeDataInfo> {
                return data.time() ? std::optional{data.time()->data_info()} : std::nullopt;
            })
        .def_property_readonly(
            "eop_info", [](const runtime::DataSnapshot &data) -> std::optional<core::EopDataInfo> {
                return data.eop() ? std::optional{data.eop()->data_info()} : std::nullopt;
            });
    py::class_<results::Trajectory>(module, "Trajectory")
        .def_property_readonly("states", &results::Trajectory::states)
        .def_property_readonly("epoch_tai_ns",
                               [](const results::Trajectory &value) {
                                   std::vector<std::int64_t> column;
                                   column.reserve(value.states().size());
                                   for (const auto &state : value.states())
                                       column.push_back(state.epoch.tai_nanoseconds());
                                   return column;
                               })
        .def_property_readonly("position_m",
                               [](const results::Trajectory &value) {
                                   std::vector<std::array<double, 3>> column;
                                   column.reserve(value.states().size());
                                   for (const auto &state : value.states())
                                       column.push_back(state.position_m);
                                   return column;
                               })
        .def_property_readonly("velocity_mps",
                               [](const results::Trajectory &value) {
                                   std::vector<std::array<double, 3>> column;
                                   column.reserve(value.states().size());
                                   for (const auto &state : value.states())
                                       column.push_back(state.velocity_mps);
                                   return column;
                               })
        .def_property_readonly("provenance", &results::Trajectory::provenance,
                               py::return_value_policy::copy)
        .def_property_readonly("object_name", &results::Trajectory::object_name)
        .def_property_readonly("origin", &results::Trajectory::origin)
        .def_property_readonly("frame", &results::Trajectory::frame)
        .def_property_readonly("input_sha256", &results::Trajectory::input_sha256)
        .def_property_readonly("data_sha256", &results::Trajectory::data_sha256)
        .def("__len__", [](const results::Trajectory &value) { return value.states().size(); })
        .def("to_csv", &runtime::export_trajectory_csv, py::arg("path"),
             py::arg("overwrite") = false, py::call_guard<py::gil_scoped_release>());
    py::class_<runtime::DataChange>(module, "DataChange")
        .def_readonly("role", &runtime::DataChange::role)
        .def_readonly("recorded", &runtime::DataChange::recorded)
        .def_readonly("selected", &runtime::DataChange::selected);
    py::class_<runtime::Analysis>(module, "Analysis")
        .def_property_readonly("snapshot", &runtime::Analysis::snapshot,
                               py::return_value_policy::copy)
        .def_property_readonly("data", &runtime::Analysis::data, py::return_value_policy::copy)
        .def("state", &runtime::Analysis::state, py::arg("object"), py::arg("at"), py::arg("frame"),
             py::call_guard<py::gil_scoped_release>())
        .def(
            "states_at",
            [](const runtime::Analysis &analysis, const application::ObjectRef &ref,
               const std::vector<core::Epoch> &times,
               core::FrameId frame) { return analysis.states(ref, times, frame); },
            py::arg("object"), py::arg("times"), py::arg("frame"),
            py::call_guard<py::gil_scoped_release>())
        .def(
            "states_step",
            [](const runtime::Analysis &analysis, const application::ObjectRef &ref,
               std::int64_t step, core::FrameId frame, std::optional<core::Epoch> first,
               std::optional<core::Epoch> last) {
                return analysis.states(ref, std::chrono::nanoseconds{step}, frame, first, last);
            },
            py::arg("object"), py::arg("step_nanoseconds"), py::arg("frame"),
            py::arg("first") = py::none(), py::arg("last") = py::none(),
            py::call_guard<py::gil_scoped_release>());
    py::class_<runtime::ProjectOpenPlan>(module, "ProjectOpenPlan")
        .def(py::init<const std::filesystem::path &, const runtime::DataCatalog &>(),
             py::arg("path"), py::arg("data"), py::call_guard<py::gil_scoped_release>())
        .def(py::init<const std::filesystem::path &, runtime::DataSnapshot>(), py::arg("path"),
             py::arg("data") = runtime::DataSnapshot{}, py::call_guard<py::gil_scoped_release>())
        .def_property_readonly("snapshot", &runtime::ProjectOpenPlan::snapshot,
                               py::return_value_policy::copy)
        .def_property_readonly("data", &runtime::ProjectOpenPlan::data,
                               py::return_value_policy::copy)
        .def_property_readonly("data_changes", &runtime::ProjectOpenPlan::data_changes,
                               py::return_value_policy::copy)
        .def("open", &runtime::ProjectOpenPlan::open, py::arg("accept_data_changes") = false,
             py::call_guard<py::gil_scoped_release>());
    py::class_<runtime::AnalysisPlan>(module, "AnalysisPlan")
        .def(py::init<const application::ProjectDocument &, const runtime::DataCatalog &>(),
             py::arg("project"), py::arg("data"), py::call_guard<py::gil_scoped_release>())
        .def(py::init<const application::ProjectDocument &, runtime::DataSnapshot>(),
             py::arg("project"), py::arg("data") = runtime::DataSnapshot{},
             py::call_guard<py::gil_scoped_release>())
        .def_property_readonly("snapshot", &runtime::AnalysisPlan::snapshot,
                               py::return_value_policy::copy)
        .def_property_readonly("data", &runtime::AnalysisPlan::data, py::return_value_policy::copy)
        .def_property_readonly("data_changes", &runtime::AnalysisPlan::data_changes,
                               py::return_value_policy::copy)
        .def("prepare", &runtime::AnalysisPlan::prepare, py::arg("accept_data_changes") = false,
             py::call_guard<py::gil_scoped_release>());
}
