#include <orbitforge/runtime/access.hpp>
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
        .value("GROUND_STATIONS", application::ReportKind::ground_stations)
        .value("CLASSICAL_ELEMENTS", application::ReportKind::classical_elements)
        .value("GEODETIC_POSITION", application::ReportKind::geodetic_position)
        .value("ORBITAL_MOTION", application::ReportKind::orbital_motion)
        .value("ACCESS_INTERVALS", application::ReportKind::access_intervals)
        .value("AER", application::ReportKind::aer)
        .value("STATE_COVARIANCE", application::ReportKind::state_covariance);
    py::class_<core::GroundObservation>(module, "GroundObservation")
        .def_readonly("azimuth_rad", &core::GroundObservation::azimuth_rad)
        .def_readonly("elevation_rad", &core::GroundObservation::elevation_rad)
        .def_readonly("range_m", &core::GroundObservation::range_m)
        .def_readonly("range_rate_mps", &core::GroundObservation::range_rate_mps)
        .def_readonly("line_of_sight", &core::GroundObservation::line_of_sight);
    module.def("observe_ground_target", &core::observe_ground_target, py::arg("station"),
               py::arg("target"));
    module.def("vector_angle", &core::vector_angle, py::arg("a"), py::arg("b"));
    py::class_<core::AccessInterval>(module, "AccessInterval")
        .def_readonly("first", &core::AccessInterval::first)
        .def_readonly("last", &core::AccessInterval::last)
        .def_readonly("start_clipped", &core::AccessInterval::start_clipped)
        .def_readonly("stop_clipped", &core::AccessInterval::stop_clipped);
    py::class_<runtime::AccessSample>(module, "AccessSample")
        .def_readonly("epoch", &runtime::AccessSample::epoch)
        .def_readonly("observation", &runtime::AccessSample::observation)
        .def_readonly("off_boresight_rad", &runtime::AccessSample::off_boresight_rad)
        .def_readonly("visible", &runtime::AccessSample::visible);
    py::class_<runtime::AccessSession>(module, "AccessSession")
        .def(py::init([](const application::Project &project, std::string station,
                         std::string target, const std::filesystem::path &eop_manifest,
                         const std::optional<std::filesystem::path> &time_file) {
                 runtime::ReportContext context{project, {}, {}, {}};
                 py::gil_scoped_release release;
                 context.time = time_file ? runtime::load_time_snapshot(*time_file)
                                          : runtime::make_builtin_time_provider();
                 context.frames = runtime::make_frame_provider();
                 context.eop = runtime::load_eop_snapshot_with_time(eop_manifest, *context.time);
                 if (context.project.earth_orientation &&
                     context.project.earth_orientation->extrapolation_days)
                     context.eop = runtime::with_eop_extrapolation(
                         context.eop, context.project.earth_orientation->extrapolation_days,
                         context.time);
                 return runtime::AccessSession{std::move(context), std::move(station),
                                               std::move(target)};
             }),
             py::arg("project"), py::arg("station_id"), py::arg("target_id"),
             py::arg("eop_manifest"), py::arg("time_file") = py::none())
        .def_property_readonly("name", &runtime::AccessSession::name)
        .def(
            "at",
            [](const runtime::AccessSession &session, core::Epoch epoch) {
                return session.at(epoch);
            },
            py::arg("epoch"), py::call_guard<py::gil_scoped_release>())
        .def(
            "intervals",
            [](const runtime::AccessSession &session, core::Epoch first, core::Epoch last,
               std::int64_t step, std::int64_t tolerance) {
                py::gil_scoped_release release;
                return core::find_access_intervals(
                    first, last, step, [&](core::Epoch epoch) { return session.at(epoch).visible; },
                    tolerance);
            },
            py::arg("first"), py::arg("last"), py::arg("step_nanoseconds") = 1'000'000'000,
            py::arg("tolerance_nanoseconds") = 1'000'000);
    py::enum_<application::ReportUnits>(module, "ReportUnits")
        .value("SI", application::ReportUnits::si)
        .value("ENGINEERING", application::ReportUnits::engineering);
    py::enum_<application::ReportTimeScale>(module, "ReportTimeScale")
        .value("UTC", application::ReportTimeScale::utc)
        .value("TAI", application::ReportTimeScale::tai)
        .value("TT", application::ReportTimeScale::tt);
    py::enum_<application::ReportScope>(module, "ReportScope")
        .value("OBJECTS", application::ReportScope::objects)
        .value("ALL_COMPATIBLE", application::ReportScope::all_compatible);
    py::class_<application::ReportQuery>(module, "ReportQuery")
        .def(py::init<>())
        .def_readwrite("kind", &application::ReportQuery::kind)
        .def_readwrite("scope", &application::ReportQuery::scope)
        .def_readwrite("object_ids", &application::ReportQuery::object_ids)
        .def_readwrite("follow_scenario", &application::ReportQuery::follow_scenario)
        .def_readwrite("first", &application::ReportQuery::first)
        .def_readwrite("last", &application::ReportQuery::last)
        .def_readwrite("step_nanoseconds", &application::ReportQuery::step_nanoseconds)
        .def_readwrite("access_search_mode", &application::ReportQuery::access_search_mode)
        .def_readwrite("access_tolerance_nanoseconds",
                       &application::ReportQuery::access_tolerance_nanoseconds)
        .def_readwrite("frame", &application::ReportQuery::frame);
    py::class_<application::ReportColumnSpec>(module, "ReportColumnSpec")
        .def(py::init<std::string, std::string, std::string>(), py::arg("field"),
             py::arg("unit") = "", py::arg("title") = "")
        .def_readwrite("field", &application::ReportColumnSpec::field)
        .def_readwrite("unit", &application::ReportColumnSpec::unit)
        .def_readwrite("title", &application::ReportColumnSpec::title);
    py::class_<application::ReportTemplate>(module, "ReportTemplate")
        .def(py::init<>())
        .def_readwrite("columns", &application::ReportTemplate::columns)
        .def_readwrite("units", &application::ReportTemplate::units)
        .def_readwrite("time_scale", &application::ReportTemplate::time_scale);
    py::class_<application::ReportDefinition>(module, "ReportDefinition")
        .def(py::init<>())
        .def_readwrite("id", &application::ReportDefinition::id)
        .def_readwrite("name", &application::ReportDefinition::name)
        .def_readwrite("query", &application::ReportDefinition::query)
        .def_readwrite("format", &application::ReportDefinition::format);
    module.def("report_columns", &application::report_columns, py::arg("kind"));
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
        .def_readonly("input_revision", &runtime::ReportResult::input_revision)
        .def_readonly("data_sha256", &runtime::ReportResult::data_sha256)
        .def_readonly("time_sha256", &runtime::ReportResult::time_sha256)
        .def_readonly("access_statistics", &runtime::ReportResult::access_statistics)
        .def_readonly("algorithm", &runtime::ReportResult::algorithm)
        .def_readonly("first", &runtime::ReportResult::first)
        .def_readonly("last", &runtime::ReportResult::last)
        .def("column_title", &runtime::ReportResult::column_title, py::arg("column"))
        .def("column_unit", &runtime::ReportResult::column_unit, py::arg("column"))
        .def("numeric_value", &runtime::ReportResult::numeric_value, py::arg("row"),
             py::arg("column"))
        .def("cell_text", &runtime::ReportResult::cell_text, py::arg("row"), py::arg("column"))
        .def(
            "with_template",
            [](const runtime::ReportResult &result, const application::ReportTemplate &format) {
                const auto captured = format;
                py::gil_scoped_release release;
                return result.with_template(captured);
            },
            py::arg("format"))
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
