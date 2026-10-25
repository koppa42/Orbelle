#include <orbelle/runtime/access.hpp>
#include <orbelle/runtime/report.hpp>
#include <orbelle/runtime/time_frames.hpp>
#include <pybind11/pybind11.h>
#include <pybind11/stl.h>
#include <pybind11/stl/filesystem.h>

namespace py = pybind11;
using namespace orbelle;
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
        .value("STATE_COVARIANCE", application::ReportKind::state_covariance)
        .value("LIGHTING_INTERVALS", application::ReportKind::lighting_intervals)
        .value("LIGHTING_STATE", application::ReportKind::lighting_state)
        .value("ORBITAL_EVENTS", application::ReportKind::orbital_events)
        .value("RELATIVE_MOTION", application::ReportKind::relative_motion)
        .value("INTERSATELLITE_ACCESS", application::ReportKind::intersatellite_access)
        .value("COVERAGE_GRID", application::ReportKind::coverage_grid)
        .value("COVERAGE_TIME", application::ReportKind::coverage_time);
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
        .def_readonly("sensor_margin_rad", &runtime::AccessSample::sensor_margin_rad)
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
    py::class_<core::CoverageGrid>(module, "CoverageGrid")
        .def(py::init<>())
        .def_readwrite("south_rad", &core::CoverageGrid::south_rad)
        .def_readwrite("north_rad", &core::CoverageGrid::north_rad)
        .def_readwrite("west_rad", &core::CoverageGrid::west_rad)
        .def_readwrite("east_rad", &core::CoverageGrid::east_rad)
        .def_readwrite("resolution_rad", &core::CoverageGrid::resolution_rad)
        .def_readwrite("minimum_elevation_rad", &core::CoverageGrid::minimum_elevation_rad)
        .def_readwrite("include_end_gaps", &core::CoverageGrid::include_end_gaps);
    py::class_<core::CoverageCell>(module, "CoverageCell")
        .def_readonly("location", &core::CoverageCell::location)
        .def_readonly("south_rad", &core::CoverageCell::south_rad)
        .def_readonly("north_rad", &core::CoverageCell::north_rad)
        .def_readonly("west_rad", &core::CoverageCell::west_rad)
        .def_readonly("east_rad", &core::CoverageCell::east_rad)
        .def_readonly("area_m2", &core::CoverageCell::area_m2);
    py::class_<core::CoverageTimeStatistics>(module, "CoverageTimeStatistics")
        .def_readonly("covered_seconds", &core::CoverageTimeStatistics::covered_seconds)
        .def_readonly("maximum_gap_seconds", &core::CoverageTimeStatistics::maximum_gap_seconds)
        .def_readonly("mean_gap_seconds", &core::CoverageTimeStatistics::mean_gap_seconds)
        .def_readonly("gap_count", &core::CoverageTimeStatistics::gap_count);
    py::class_<results::CoveragePoint>(module, "CoveragePoint")
        .def_readonly("cell", &results::CoveragePoint::cell)
        .def_readonly("windows", &results::CoveragePoint::windows)
        .def_readonly("statistics", &results::CoveragePoint::statistics);
    py::class_<results::CoverageStatistics>(module, "CoverageStatistics")
        .def_property_readonly("grid", [](const results::CoverageStatistics &s) { return s.grid; })
        .def_property_readonly("region",
                               [](const results::CoverageStatistics &s) { return s.region; })
        .def_readonly("points", &results::CoverageStatistics::points)
        .def_property_readonly("point_count",
                               [](const results::CoverageStatistics &s) { return s.points.size(); })
        .def(
            "iter_points",
            [](const results::CoverageStatistics &s) {
                return py::make_iterator<py::return_value_policy::copy>(s.points.begin(),
                                                                        s.points.end());
            },
            py::keep_alive<0, 1>())
        .def_readonly("total_area_m2", &results::CoverageStatistics::total_area_m2)
        .def_readonly("ever_covered_area_m2", &results::CoverageStatistics::ever_covered_area_m2)
        .def_readonly("mean_coverage_fraction",
                      &results::CoverageStatistics::mean_coverage_fraction)
        .def_readonly("maximum_gap_seconds", &results::CoverageStatistics::maximum_gap_seconds);
    py::class_<application::ReportQuery>(module, "ReportQuery")
        .def(py::init<>())
        .def_readwrite("kind", &application::ReportQuery::kind)
        .def_readwrite("scope", &application::ReportQuery::scope)
        .def_readwrite("object_ids", &application::ReportQuery::object_ids)
        .def_readwrite("region_id", &application::ReportQuery::region_id)
        .def_readwrite("follow_scenario", &application::ReportQuery::follow_scenario)
        .def_readwrite("first", &application::ReportQuery::first)
        .def_readwrite("last", &application::ReportQuery::last)
        .def_readwrite("step_nanoseconds", &application::ReportQuery::step_nanoseconds)
        .def_readwrite("event_search_mode", &application::ReportQuery::event_search_mode)
        .def_readwrite("event_tolerance_nanoseconds",
                       &application::ReportQuery::event_tolerance_nanoseconds)
        .def_readwrite("frame", &application::ReportQuery::frame)
        .def_readwrite("lighting_ephemeris", &application::ReportQuery::lighting_ephemeris)
        .def_readwrite("coverage", &application::ReportQuery::coverage);
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
    py::class_<results::LightingPhaseStatistics>(module, "LightingPhaseStatistics")
        .def_readonly("count", &results::LightingPhaseStatistics::count)
        .def_readonly("total_duration_s", &results::LightingPhaseStatistics::total_duration_s)
        .def_readonly("longest_duration_s", &results::LightingPhaseStatistics::longest_duration_s)
        .def_readonly("fraction", &results::LightingPhaseStatistics::fraction);
    py::class_<results::LightingStatistics>(module, "LightingStatistics")
        .def_readonly("object_count", &results::LightingStatistics::object_count)
        .def_property_readonly("sunlight",
                               [](const results::LightingStatistics &s) { return s.phases[0]; })
        .def_property_readonly("penumbra",
                               [](const results::LightingStatistics &s) { return s.phases[1]; })
        .def_property_readonly("umbra",
                               [](const results::LightingStatistics &s) { return s.phases[2]; });
    py::class_<results::OrbitalEventStatistics>(module, "OrbitalEventStatistics")
        .def_readonly("object_count", &results::OrbitalEventStatistics::object_count)
        .def_readonly("counts", &results::OrbitalEventStatistics::counts)
        .def_readonly("near_circular_objects",
                      &results::OrbitalEventStatistics::near_circular_objects)
        .def_readonly("near_equatorial_objects",
                      &results::OrbitalEventStatistics::near_equatorial_objects);
    py::class_<results::IntersatelliteStatistics>(module, "IntersatelliteStatistics")
        .def_readonly("minimum_range", &results::IntersatelliteStatistics::minimum_range)
        .def_readonly("maximum_range", &results::IntersatelliteStatistics::maximum_range)
        .def_readonly("visibility", &results::IntersatelliteStatistics::visibility)
        .def_readonly("windows", &results::IntersatelliteStatistics::windows);
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
        .def_readonly("lighting_statistics", &runtime::ReportResult::lighting_statistics)
        .def_readonly("orbital_event_statistics", &runtime::ReportResult::orbital_event_statistics)
        .def_readonly("intersatellite_statistics",
                      &runtime::ReportResult::intersatellite_statistics)
        .def_readonly("coverage_statistics", &runtime::ReportResult::coverage_statistics)
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
               py::arg("definition"),
               py::arg("coverage_memory_bytes") = runtime::default_coverage_memory_bytes);
    py::class_<runtime::CoverageReportEstimate>(module, "CoverageReportEstimate")
        .def_readonly("points", &runtime::CoverageReportEstimate::points)
        .def_readonly("rows", &runtime::CoverageReportEstimate::rows)
        .def_readonly("storage_bytes", &runtime::CoverageReportEstimate::storage_bytes);
    module.def(
        "estimate_coverage_report",
        [](const application::Project &project, const application::ReportDefinition &definition,
           std::size_t memory_bytes) {
            return runtime::estimate_coverage_report(project, definition, memory_bytes);
        },
        py::arg("project"), py::arg("definition"),
        py::arg("coverage_memory_bytes") = runtime::default_coverage_memory_bytes);
    module.def("report_input_sha256", &runtime::report_input_sha256, py::arg("project"));
    module.def(
        "generate_report",
        [](const application::Project &project, const application::ReportDefinition &definition,
           const std::optional<std::filesystem::path> &eop_manifest,
           const std::optional<std::filesystem::path> &time_file,
           std::size_t coverage_memory_bytes) {
            // Copy mutable Python inputs before releasing the GIL.
            runtime::ReportContext snapshot{project, {}, {}, {}};
            snapshot.coverage_memory_bytes = coverage_memory_bytes;
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
        py::arg("time_file") = py::none(),
        py::arg("coverage_memory_bytes") = runtime::default_coverage_memory_bytes);
}
