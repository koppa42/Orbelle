#include <orbitforge/runtime/report.hpp>
#include <orbitforge/runtime/time_frames.hpp>
#include <pybind11/pybind11.h>
#include <pybind11/stl.h>
#include <pybind11/stl/filesystem.h>

namespace py = pybind11;
using namespace orbitforge;
void bind_reports(py::module_ &module) {
    py::enum_<application::ReportKind>(module, "ReportKind")
        .value("SCENARIO_SUMMARY", application::ReportKind::scenario_summary)
        .value("OBJECT_SUMMARY", application::ReportKind::object_summary)
        .value("CARTESIAN_STATE", application::ReportKind::cartesian_state)
        .value("GROUND_STATIONS", application::ReportKind::ground_stations);
    py::enum_<application::ReportUnits>(module, "ReportUnits")
        .value("SI", application::ReportUnits::si)
        .value("ENGINEERING", application::ReportUnits::engineering);
    py::enum_<application::ReportTimeScale>(module, "ReportTimeScale")
        .value("UTC", application::ReportTimeScale::utc)
        .value("TAI", application::ReportTimeScale::tai)
        .value("TT", application::ReportTimeScale::tt);
    py::class_<application::ReportDefinition>(module, "ReportDefinition")
        .def(py::init<>())
        .def_readwrite("id", &application::ReportDefinition::id)
        .def_readwrite("name", &application::ReportDefinition::name)
        .def_readwrite("kind", &application::ReportDefinition::kind)
        .def_readwrite("object_ids", &application::ReportDefinition::object_ids)
        .def_readwrite("follow_scenario", &application::ReportDefinition::follow_scenario)
        .def_readwrite("first", &application::ReportDefinition::first)
        .def_readwrite("last", &application::ReportDefinition::last)
        .def_readwrite("step_nanoseconds", &application::ReportDefinition::step_nanoseconds)
        .def_readwrite("frame", &application::ReportDefinition::frame)
        .def_readwrite("units", &application::ReportDefinition::units)
        .def_readwrite("time_scale", &application::ReportDefinition::time_scale);
    py::class_<results::Column>(module, "ReportColumn")
        .def_readonly("id", &results::Column::id)
        .def_readonly("title", &results::Column::title)
        .def_readonly("unit", &results::Column::unit);
    py::class_<results::Table>(module, "ReportTable")
        .def_property_readonly("columns",
                               [](const results::Table &table) { return table.columns(); })
        .def_property_readonly("rows", [](const results::Table &table) { return table.rows(); })
        .def_property_readonly("row_count",
                               [](const results::Table &table) { return table.rows().size(); });
    py::class_<runtime::ReportResult>(module, "ReportResult")
        .def_property_readonly("definition",
                               [](const runtime::ReportResult &r) { return r.definition; })
        .def_readonly("table", &runtime::ReportResult::table)
        .def_readonly("input_sha256", &runtime::ReportResult::input_sha256)
        .def_readonly("data_sha256", &runtime::ReportResult::data_sha256)
        .def_readonly("time_sha256", &runtime::ReportResult::time_sha256)
        .def_readonly("algorithm", &runtime::ReportResult::algorithm)
        .def_readonly("first", &runtime::ReportResult::first)
        .def_readonly("last", &runtime::ReportResult::last)
        .def("column_title", &runtime::ReportResult::column_title, py::arg("column"))
        .def("cell_text", &runtime::ReportResult::cell_text, py::arg("row"), py::arg("column"))
        .def(
            "export_csv",
            [](const runtime::ReportResult &r, const std::filesystem::path &path, bool overwrite) {
                py::gil_scoped_release release;
                runtime::export_report_csv(r, path, overwrite);
            },
            py::arg("path"), py::arg("overwrite") = false);
    module.def("estimate_report_rows", &runtime::estimate_report_rows, py::arg("project"),
               py::arg("definition"));
    module.def("report_input_sha256", &runtime::report_input_sha256, py::arg("project"));
    module.def(
        "generate_report",
        [](const application::Project &project, const application::ReportDefinition &definition,
           const std::optional<std::filesystem::path> &eop_manifest,
           const std::optional<std::filesystem::path> &time_file) {
            // Copy mutable Python inputs before releasing the GIL.
            runtime::ReportContext snapshot{project, {}, {}, {}};
            const auto requested = definition;
            py::gil_scoped_release release;
            if (runtime::time_frames_available()) {
                snapshot.time = !time_file ? runtime::make_builtin_time_provider()
                                           : runtime::load_time_snapshot(*time_file);
                snapshot.frames = runtime::make_frame_provider();
            } else if (time_file || eop_manifest)
                throw std::runtime_error("Report time/frame support is unavailable in this build");
            if (eop_manifest) {
                snapshot.eop = runtime::load_eop_snapshot_with_time(*eop_manifest, *snapshot.time);
                if (snapshot.project.earth_orientation &&
                    snapshot.project.earth_orientation->extrapolation_days > 0)
                    snapshot.eop = runtime::with_eop_extrapolation(
                        snapshot.eop, snapshot.project.earth_orientation->extrapolation_days,
                        snapshot.time);
            }
            return runtime::generate_report(snapshot, requested);
        },
        py::arg("project"), py::arg("definition"), py::arg("eop_manifest") = py::none(),
        py::arg("time_file") = py::none());
}
