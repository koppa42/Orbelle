#include <algorithm>
#include <orbitforge/core/propagation/hpop_covariance.hpp>
#include <orbitforge/runtime/analysis.hpp>
#include <orbitforge/runtime/data_catalog.hpp>
#include <orbitforge/runtime/hpop.hpp>
#include <pybind11/pybind11.h>
#include <pybind11/stl.h>
#include <pybind11/stl/filesystem.h>

namespace py = pybind11;
using namespace orbitforge;
namespace {
// The Python callable remains owned by the binding's argument until the GIL is restored.
std::function<bool()> cancellation(const py::object &callback) {
    if (callback.is_none())
        return {};
    if (!PyCallable_Check(callback.ptr()))
        throw py::type_error("cancelled must be callable or None");
    return [&callback] {
        py::gil_scoped_acquire acquire;
        const auto value = callback();
        if (!py::isinstance<py::bool_>(value))
            throw py::type_error("cancelled must return bool");
        return value.cast<bool>();
    };
}
std::function<void(std::size_t, std::size_t)> progress_callback(const py::object &callback) {
    if (callback.is_none())
        return {};
    if (!PyCallable_Check(callback.ptr()))
        throw py::type_error("progress must be callable or None");
    return [&callback](std::size_t current, std::size_t total) {
        py::gil_scoped_acquire acquire;
        callback(current, total);
    };
}
application::OrbitInput variational_resources(application::OrbitInput input,
                                              const core::HpopVariationalOptions &options) {
    // Covariance gravity may activate tide resources absent from a nominal 0/0 field.
    if (options.gravity) {
        input.hpop.gravity_degree = std::max(input.hpop.gravity_degree, options.gravity->degree);
        input.hpop.gravity_order = std::max(input.hpop.gravity_order, options.gravity->order);
    }
    return input;
}
} // namespace
void bind_analysis(py::module_ &module) {
    module.attr("MAX_ANALYSIS_SAMPLES") = runtime::max_analysis_samples;
    py::register_exception<runtime::AnalysisCapabilityError>(module, "AnalysisCapabilityError",
                                                             PyExc_RuntimeError);
    py::register_exception<runtime::AnalysisCancelled>(module, "AnalysisCancelled",
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
        .def("with_space_weather", &runtime::DataSnapshot::with_space_weather_file,
             py::arg("model"), py::arg("file"), py::call_guard<py::gil_scoped_release>())
        .def("with_ocean_tides", &runtime::DataSnapshot::with_ocean_tide_file, py::arg("model"),
             py::arg("file"), py::call_guard<py::gil_scoped_release>())
        .def("with_gravity", &runtime::DataSnapshot::with_gravity_file, py::arg("model"),
             py::arg("file"), py::call_guard<py::gil_scoped_release>())
        .def("with_ephemeris", &runtime::DataSnapshot::with_ephemeris_file, py::arg("model"),
             py::arg("manifest"), py::arg("workspace"), py::call_guard<py::gil_scoped_release>())
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
    py::enum_<core::AccessSearchMode>(module, "AccessSearchMode")
        .value("FIXED_GRID", core::AccessSearchMode::fixed_grid)
        .value("EXTREMUM_ASSISTED", core::AccessSearchMode::extremum_assisted);
    py::class_<core::AccessExtremum>(module, "AccessExtremum")
        .def_readonly("epoch", &core::AccessExtremum::epoch)
        .def_readonly("value", &core::AccessExtremum::value);
    py::class_<core::PassExtrema>(module, "PassExtrema")
        .def_readonly("maximum_elevation", &core::PassExtrema::maximum_elevation)
        .def_readonly("minimum_range", &core::PassExtrema::minimum_range);
    py::class_<results::AccessStatistics>(module, "AccessStatistics")
        .def_readonly("count", &results::AccessStatistics::count)
        .def_readonly("total_duration_s", &results::AccessStatistics::total_duration_s)
        .def_readonly("mean_duration_s", &results::AccessStatistics::mean_duration_s)
        .def_readonly("shortest_duration_s", &results::AccessStatistics::shortest_duration_s)
        .def_readonly("longest_duration_s", &results::AccessStatistics::longest_duration_s);
    py::class_<results::AccessWindow>(module, "AccessWindow")
        .def_property_readonly("extrema", &results::AccessWindow::extrema)
        .def_property_readonly("start", &results::AccessWindow::start)
        .def_property_readonly("end", &results::AccessWindow::end)
        .def_property_readonly(
            "duration_nanoseconds",
            [](const results::AccessWindow &window) { return window.duration().count(); })
        .def_property_readonly("start_clipped", &results::AccessWindow::start_clipped)
        .def_property_readonly("end_clipped", &results::AccessWindow::end_clipped);
    py::class_<results::AccessProvenance>(module, "AccessProvenance")
        .def_readonly("station_id", &results::AccessProvenance::station_id)
        .def_readonly("station_name", &results::AccessProvenance::station_name)
        .def_readonly("target_id", &results::AccessProvenance::target_id)
        .def_readonly("target_name", &results::AccessProvenance::target_name)
        .def_readonly("input_revision", &results::AccessProvenance::input_revision)
        .def_readonly("input_sha256", &results::AccessProvenance::input_sha256)
        .def_readonly("data_sha256", &results::AccessProvenance::data_sha256)
        .def_readonly("algorithm", &results::AccessProvenance::algorithm);
    py::class_<results::AccessWindows>(module, "AccessWindows")
        .def_property_readonly(
            "search_mode", [](const results::AccessWindows &v) { return v.query().search_mode; })
        .def_property_readonly("statistics", &results::AccessWindows::statistics)
        .def_property_readonly("windows", &results::AccessWindows::windows)
        .def("__len__", [](const results::AccessWindows &value) { return value.windows().size(); })
        .def("window", [](const results::AccessWindows &value,
                          std::size_t index) { return value.windows().at(index); })
        .def_property_readonly("provenance", &results::AccessWindows::provenance,
                               py::return_value_policy::copy)
        .def_property_readonly(
            "first", [](const results::AccessWindows &value) { return value.query().first; })
        .def_property_readonly(
            "last", [](const results::AccessWindows &value) { return value.query().last; })
        .def_property_readonly(
            "search_step_nanoseconds",
            [](const results::AccessWindows &value) { return value.query().search_step.count(); })
        .def_property_readonly("boundary_tolerance_nanoseconds",
                               [](const results::AccessWindows &value) {
                                   return value.query().boundary_tolerance.count();
                               })
        .def("to_csv", &runtime::export_access_csv, py::arg("path"), py::arg("overwrite") = false,
             py::call_guard<py::gil_scoped_release>());
    py::class_<runtime::DataChange>(module, "DataChange")
        .def_readonly("role", &runtime::DataChange::role)
        .def_readonly("recorded", &runtime::DataChange::recorded)
        .def_readonly("selected", &runtime::DataChange::selected);
    py::class_<results::AerSeries>(module, "AerSeries")
        .def_property_readonly("samples", &results::AerSeries::samples)
        .def("sample", [](const results::AerSeries &value,
                          std::size_t index) { return value.samples().at(index); })
        .def("__len__", [](const results::AerSeries &value) { return value.samples().size(); })
        .def_property_readonly("provenance", &results::AerSeries::provenance,
                               py::return_value_policy::copy)
        .def("to_csv", &runtime::export_aer_csv, py::arg("path"), py::arg("overwrite") = false,
             py::call_guard<py::gil_scoped_release>());
    py::class_<core::ManeuverEvent>(module, "ManeuverEvent")
        .def_readonly("segment_index", &core::ManeuverEvent::segment_index)
        .def_readonly("name", &core::ManeuverEvent::name)
        .def_readonly("before", &core::ManeuverEvent::before)
        .def_readonly("after", &core::ManeuverEvent::after)
        .def_readonly("delta_v_gcrf_mps", &core::ManeuverEvent::delta_v_gcrf_mps)
        .def_readonly("cumulative_delta_v_mps", &core::ManeuverEvent::cumulative_delta_v_mps);
    py::class_<results::ManeuverSeries>(module, "ManeuverSeries")
        .def_property_readonly("events", &results::ManeuverSeries::events)
        .def_property_readonly("provenance", &results::ManeuverSeries::provenance,
                               py::return_value_policy::copy)
        .def_property_readonly("input_sha256", &results::ManeuverSeries::input_sha256)
        .def_property_readonly("data_sha256", &results::ManeuverSeries::data_sha256)
        .def("__len__", [](const results::ManeuverSeries &value) { return value.events().size(); });
    py::class_<runtime::Analysis>(module, "Analysis")
        .def(
            "covariance",
            [](runtime::Analysis analysis, application::ObjectRef ref, core::Epoch at,
               const py::object &cancelled) {
                const auto check = cancellation(cancelled);
                py::gil_scoped_release release;
                return analysis.covariance(ref, at, check);
            },
            py::arg("satellite"), py::arg("at"), py::arg("cancelled") = py::none())
        .def(
            "maneuvers",
            [](const runtime::Analysis &analysis, const application::ObjectRef &ref,
               const py::object &cancelled) {
                const auto check = cancellation(cancelled);
                py::gil_scoped_release release;
                return analysis.maneuvers(ref, check);
            },
            py::arg("satellite"), py::arg("cancelled") = py::none())
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
            py::call_guard<py::gil_scoped_release>())
        .def(
            "access",
            [](const runtime::Analysis &analysis, const application::ObjectRef &station,
               const application::ObjectRef &target, std::int64_t step, std::int64_t tolerance,
               core::AccessSearchMode mode, std::optional<core::Epoch> first,
               std::optional<core::Epoch> last, const py::object &cancelled) {
                const auto check = cancellation(cancelled);
                py::gil_scoped_release release;
                return analysis.access(station, target,
                                       {std::chrono::nanoseconds{step},
                                        std::chrono::nanoseconds{tolerance}, first, last, mode},
                                       check);
            },
            py::arg("station"), py::arg("target"), py::kw_only(),
            py::arg("search_step_nanoseconds"),
            py::arg("boundary_tolerance_nanoseconds") = 1'000'000,
            py::arg("search_mode") = core::AccessSearchMode::extremum_assisted,
            py::arg("first") = py::none(), py::arg("last") = py::none(),
            py::arg("cancelled") = py::none())
        .def(
            "aer_at",
            [](const runtime::Analysis &analysis, const application::ObjectRef &station,
               const application::ObjectRef &target, const std::vector<core::Epoch> &times,
               const py::object &cancelled) {
                const auto check = cancellation(cancelled);
                py::gil_scoped_release release;
                return analysis.aer(station, target, times, check);
            },
            py::arg("station"), py::arg("target"), py::arg("times"),
            py::arg("cancelled") = py::none())
        .def(
            "aer_step",
            [](const runtime::Analysis &analysis, const application::ObjectRef &station,
               const application::ObjectRef &target, std::int64_t step,
               std::optional<core::Epoch> first, std::optional<core::Epoch> last,
               const py::object &cancelled) {
                const auto check = cancellation(cancelled);
                py::gil_scoped_release release;
                return analysis.aer(station, target, std::chrono::nanoseconds{step}, first, last,
                                    check);
            },
            py::arg("station"), py::arg("target"), py::arg("step_nanoseconds"),
            py::arg("first") = py::none(), py::arg("last") = py::none(),
            py::arg("cancelled") = py::none())
        .def(
            "report",
            [](const runtime::Analysis &analysis, const application::ReportDefinition &definition,
               const py::object &cancelled, const py::object &progress) {
                const auto captured = definition;
                const auto check = cancellation(cancelled);
                const auto notify = progress_callback(progress);
                py::gil_scoped_release release;
                return analysis.report(captured, check, notify);
            },
            py::arg("definition"), py::arg("cancelled") = py::none(),
            py::arg("progress") = py::none());
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
    py::class_<core::HpopGravityTruncation>(module, "HpopGravityTruncation")
        .def(py::init<>())
        .def(py::init<int, int>(), py::arg("degree"), py::arg("order"))
        .def_readwrite("degree", &core::HpopGravityTruncation::degree)
        .def_readwrite("order", &core::HpopGravityTruncation::order);
    py::class_<core::HpopVariationalOptions>(module, "HpopVariationalOptions")
        .def(py::init<>())
        .def_readwrite("gravity", &core::HpopVariationalOptions::gravity)
        .def_readwrite("absolute_tolerance", &core::HpopVariationalOptions::absolute_tolerance);
    py::class_<core::HpopVariationalState>(module, "HpopVariationalState")
        .def_property_readonly("state", [](const core::HpopVariationalState &v) { return v.state; })
        .def_readonly("state_transition", &core::HpopVariationalState::state_transition)
        .def_readonly("drag_sensitivity", &core::HpopVariationalState::drag_sensitivity)
        .def_readonly("srp_sensitivity", &core::HpopVariationalState::srp_sensitivity);
    module.def(
        "propagate_hpop_variational",
        [](application::OrbitInput input, core::Epoch end, core::HpopVariationalOptions options,
           runtime::DataSnapshot data, py::object callback) {
            if (input.propagator != application::OrbitPropagator::hpop)
                throw std::invalid_argument("Variational propagation requires one HPOP coast");
            const auto cancelled = cancellation(callback);
            py::gil_scoped_release release;
            return core::propagate_hpop_variational(
                input.initial, end, input.hpop,
                data.force_environment(variational_resources(input, options)), options, [&] {
                    if (cancelled && cancelled())
                        throw runtime::AnalysisCancelled{};
                });
        },
        py::arg("input"), py::arg("end"), py::kw_only(),
        py::arg("options") = core::HpopVariationalOptions{},
        py::arg("data") = runtime::DataSnapshot{}, py::arg("cancelled") = py::none());
    py::enum_<core::HpopCovarianceFrame>(module, "HpopCovarianceFrame")
        .value("GCRF", core::HpopCovarianceFrame::gcrf)
        .value("LVLH", core::HpopCovarianceFrame::lvlh)
        .value("FRENET", core::HpopCovarianceFrame::frenet)
        .value("FK5_J2000", core::HpopCovarianceFrame::fk5_j2000)
        .value("TRUE_OF_DATE", core::HpopCovarianceFrame::true_of_date);
    py::enum_<core::HpopCovarianceVelocity>(module, "HpopCovarianceVelocity")
        .value("PROJECTED", core::HpopCovarianceVelocity::projected)
        .value("ROTATING", core::HpopCovarianceVelocity::rotating);
    py::class_<core::HpopCovarianceCoordinates>(module, "HpopCovarianceCoordinates")
        .def(py::init<>())
        .def(py::init<core::HpopCovarianceFrame, core::HpopCovarianceVelocity>(), py::arg("frame"),
             py::arg("velocity"))
        .def_readwrite("frame", &core::HpopCovarianceCoordinates::frame)
        .def_readwrite("velocity", &core::HpopCovarianceCoordinates::velocity);
    module.def(
        "hpop_covariance_frame_jacobian",
        [](core::CartesianState reference, core::HpopCovarianceCoordinates source,
           core::HpopCovarianceCoordinates target,
           std::optional<std::array<double, 3>> acceleration, runtime::DataSnapshot data) {
            py::gil_scoped_release release;
            return core::hpop_covariance_frame_jacobian(reference, source, target, acceleration,
                                                        {data.frames(), data.eop()});
        },
        py::arg("reference"), py::arg("source"), py::arg("target"), py::kw_only(),
        py::arg("acceleration_mps2") = py::none(), py::arg("data") = runtime::DataSnapshot{});
    py::class_<core::HpopConsiderParameter>(module, "HpopConsiderParameter")
        .def(py::init<>())
        .def_readwrite("variance", &core::HpopConsiderParameter::variance)
        .def_readwrite("state_cross_covariance",
                       &core::HpopConsiderParameter::state_cross_covariance);
    py::class_<core::HpopCovarianceInput>(module, "HpopCovarianceInput")
        .def(py::init<>())
        .def_readwrite("state", &core::HpopCovarianceInput::state)
        .def_readwrite("coordinates", &core::HpopCovarianceInput::coordinates)
        .def_readwrite("drag", &core::HpopCovarianceInput::drag)
        .def_readwrite("srp", &core::HpopCovarianceInput::srp)
        .def_readwrite("drag_srp_covariance", &core::HpopCovarianceInput::drag_srp_covariance);
    py::class_<core::HpopCovarianceContributions>(module, "HpopCovarianceContributions")
        .def_readonly("initial_state", &core::HpopCovarianceContributions::initial_state)
        .def_readonly("consider_parameters",
                      &core::HpopCovarianceContributions::consider_parameters)
        .def_readonly("state_parameter_cross",
                      &core::HpopCovarianceContributions::state_parameter_cross);
    py::class_<core::HpopCovarianceState>(module, "HpopCovarianceState")
        .def_property_readonly("variational",
                               [](const core::HpopCovarianceState &s) { return s.variational; })
        .def_property_readonly("covariance",
                               [](const core::HpopCovarianceState &s) { return s.covariance; })
        .def_property_readonly("contributions",
                               [](const core::HpopCovarianceState &s) { return s.contributions; });
    py::class_<application::HpopCovarianceSettings>(module, "HpopCovarianceSettings")
        .def(py::init<>())
        .def_readwrite("enabled", &application::HpopCovarianceSettings::enabled)
        .def_readwrite("state", &application::HpopCovarianceSettings::state)
        .def_readwrite("coordinates", &application::HpopCovarianceSettings::coordinates)
        .def_readwrite("independent_gravity",
                       &application::HpopCovarianceSettings::independent_gravity)
        .def_readwrite("gravity", &application::HpopCovarianceSettings::gravity)
        .def_readwrite("absolute_tolerance",
                       &application::HpopCovarianceSettings::absolute_tolerance)
        .def_readwrite("consider_analysis", &application::HpopCovarianceSettings::consider_analysis)
        .def_readwrite("drag_enabled", &application::HpopCovarianceSettings::drag_enabled)
        .def_readwrite("srp_enabled", &application::HpopCovarianceSettings::srp_enabled)
        .def_readwrite("drag", &application::HpopCovarianceSettings::drag)
        .def_readwrite("srp", &application::HpopCovarianceSettings::srp)
        .def_readwrite("consider_cross_correlation",
                       &application::HpopCovarianceSettings::consider_cross_correlation)
        .def_readwrite("drag_srp_covariance",
                       &application::HpopCovarianceSettings::drag_srp_covariance)
        .def("active_input", &application::HpopCovarianceSettings::active_input)
        .def("variational_options", &application::HpopCovarianceSettings::variational_options)
        .def("validate", &application::validate_hpop_covariance_settings,
             py::arg("force") = core::HpopOptions{});
    module.def("validate_hpop_covariance", &core::validate_hpop_covariance, py::arg("input"));
    module.def(
        "convert_hpop_covariance",
        [](core::CartesianState reference, core::HpopCovarianceInput input,
           core::HpopCovarianceCoordinates target,
           std::optional<std::array<double, 3>> acceleration, runtime::DataSnapshot data) {
            py::gil_scoped_release release;
            return core::convert_hpop_covariance(reference, input, target, acceleration, {},
                                                 {data.frames(), data.eop()});
        },
        py::arg("reference"), py::arg("input"), py::arg("target"), py::kw_only(),
        py::arg("acceleration_mps2") = py::none(), py::arg("data") = runtime::DataSnapshot{});
    module.def(
        "transport_hpop_covariance",
        [](core::HpopCovarianceInput input, core::HpopVariationalState state) {
            py::gil_scoped_release release;
            return core::transport_hpop_covariance(input, state);
        },
        py::arg("input"), py::arg("variational"));
    module.def(
        "propagate_hpop_covariance",
        [](application::OrbitInput input, core::Epoch end, core::HpopCovarianceInput covariance,
           core::HpopVariationalOptions options, runtime::DataSnapshot data, py::object callback) {
            if (input.propagator != application::OrbitPropagator::hpop)
                throw std::invalid_argument("Covariance propagation requires one HPOP coast");
            const auto cancelled = cancellation(callback);
            py::gil_scoped_release release;
            const auto check = [&] {
                if (cancelled && cancelled())
                    throw runtime::AnalysisCancelled{};
            };
            check();
            core::validate_hpop_covariance(covariance);
            return core::propagate_hpop_covariance(
                input.initial, end, covariance, input.hpop,
                data.force_environment(variational_resources(input, options)), options, check);
        },
        py::arg("input"), py::arg("end"), py::arg("covariance"), py::kw_only(),
        py::arg("options") = core::HpopVariationalOptions{},
        py::arg("data") = runtime::DataSnapshot{}, py::arg("cancelled") = py::none());
    module.def(
        "propagate_hpop_ephemeris",
        [](application::OrbitInput input, core::Epoch end, std::int64_t report_ns,
           runtime::DataSnapshot data, py::object callback) {
            if (input.propagator != application::OrbitPropagator::hpop)
                throw std::invalid_argument(
                    "Ephemeris generation requires one HPOP coast; split missions at impulses");
            const auto cancelled = cancellation(callback);
            py::gil_scoped_release release;
            return core::propagate_hpop_ephemeris(input.initial, end, input.hpop,
                                                  data.force_environment(input), report_ns, [&] {
                                                      if (cancelled && cancelled())
                                                          throw runtime::AnalysisCancelled{};
                                                  });
        },
        py::arg("input"), py::arg("end"), py::kw_only(), py::arg("reporting_step_nanoseconds") = 0,
        py::arg("data") = runtime::DataSnapshot{}, py::arg("cancelled") = py::none());
    module.def(
        "make_hpop_ephemeris_session",
        [](application::OrbitInput input, core::Epoch end,
           core::OrbitInterpolationOptions interpolation, std::int64_t report_ns,
           runtime::DataSnapshot data, std::uint64_t revision, py::object callback) {
            if (input.propagator != application::OrbitPropagator::hpop)
                throw std::invalid_argument(
                    "Ephemeris generation requires one HPOP coast; split missions at impulses");
            const auto cancelled = cancellation(callback);
            py::gil_scoped_release release;
            return runtime::make_hpop_ephemeris_session(
                input.initial, end, input.hpop, data.force_environment(input), interpolation,
                report_ns, input.object_id, revision, [&] {
                    if (cancelled && cancelled())
                        throw runtime::AnalysisCancelled{};
                });
        },
        py::arg("input"), py::arg("end"), py::arg("interpolation"), py::kw_only(),
        py::arg("reporting_step_nanoseconds") = 0, py::arg("data") = runtime::DataSnapshot{},
        py::arg("input_revision") = 1, py::arg("cancelled") = py::none());
}
