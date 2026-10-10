#include <orbitforge/runtime/time_frames.hpp>
#include <pybind11/pybind11.h>
#include <pybind11/stl.h>
#include <pybind11/stl/filesystem.h>

namespace py = pybind11;
using namespace orbitforge;

void bind_time_frames(py::module_ &module) {
    py::register_exception<core::EopDataError>(module, "EopDataError", PyExc_ValueError);
    py::register_exception<core::EopCoverageError>(module, "EopCoverageError", PyExc_ValueError);
    py::register_exception<core::TimeDataRangeError>(module, "TimeDataRangeError",
                                                     PyExc_ValueError);
    py::class_<core::UtcDateTime>(module, "UtcDateTime")
        .def(py::init<int, int, int, int, int, int, int>(), py::arg("year"), py::arg("month"),
             py::arg("day"), py::arg("hour") = 0, py::arg("minute") = 0, py::arg("second") = 0,
             py::arg("nanosecond") = 0)
        .def_readonly("year", &core::UtcDateTime::year)
        .def_readonly("month", &core::UtcDateTime::month)
        .def_readonly("day", &core::UtcDateTime::day)
        .def_readonly("hour", &core::UtcDateTime::hour)
        .def_readonly("minute", &core::UtcDateTime::minute)
        .def_readonly("second", &core::UtcDateTime::second)
        .def_readonly("nanosecond", &core::UtcDateTime::nanosecond);
    py::enum_<core::TimeScale>(module, "TimeScale")
        .value("TAI", core::TimeScale::tai)
        .value("TT", core::TimeScale::tt)
        .value("TDB", core::TimeScale::tdb);
    py::class_<core::JulianDate>(module, "JulianDate")
        .def_readonly("scale", &core::JulianDate::scale)
        .def_readonly("day", &core::JulianDate::day)
        .def_readonly("fraction", &core::JulianDate::fraction);
    py::class_<core::TimeDataInfo>(module, "TimeDataInfo")
        .def_readonly("id", &core::TimeDataInfo::id)
        .def_readonly("sha256", &core::TimeDataInfo::sha256)
        .def_readonly("source_url", &core::TimeDataInfo::source_url)
        .def_readonly("utc_valid_from_mjd", &core::TimeDataInfo::utc_valid_from_mjd)
        .def_readonly("utc_valid_until_mjd", &core::TimeDataInfo::utc_valid_until_mjd);
    py::class_<core::ITimeProvider>(module, "TimeProvider")
        .def("from_utc", &core::ITimeProvider::from_utc, py::arg("utc"))
        .def("to_utc", &core::ITimeProvider::to_utc, py::arg("epoch"))
        .def("tai_minus_utc_seconds", &core::ITimeProvider::tai_minus_utc_seconds, py::arg("epoch"))
        .def("julian_date", &core::ITimeProvider::julian_date, py::arg("epoch"), py::arg("scale"))
        .def_property_readonly("data_info", &core::ITimeProvider::data_info,
                               py::return_value_policy::copy);
    py::class_<core::EarthOrientationSample>(module, "EarthOrientationSample")
        .def(py::init<core::Epoch, double, double, double, double, double, std::string>(),
             py::arg("epoch"), py::arg("ut1_minus_tai_seconds"), py::arg("polar_motion_x_rad"),
             py::arg("polar_motion_y_rad"), py::arg("celestial_pole_dx_rad"),
             py::arg("celestial_pole_dy_rad"), py::arg("source_id"))
        .def_readonly("epoch", &core::EarthOrientationSample::epoch)
        .def_readonly("ut1_minus_tai_seconds", &core::EarthOrientationSample::ut1_minus_tai_seconds)
        .def_readonly("polar_motion_x_rad", &core::EarthOrientationSample::polar_motion_x_rad)
        .def_readonly("polar_motion_y_rad", &core::EarthOrientationSample::polar_motion_y_rad)
        .def_readonly("celestial_pole_dx_rad", &core::EarthOrientationSample::celestial_pole_dx_rad)
        .def_readonly("celestial_pole_dy_rad", &core::EarthOrientationSample::celestial_pole_dy_rad)
        .def_readonly("source_id", &core::EarthOrientationSample::source_id);
    py::class_<core::EarthOrientationRates>(module, "EarthOrientationRates")
        .def(py::init<double, double, double, double, double>(),
             py::arg("ut1_minus_tai_seconds_per_second"), py::arg("polar_motion_x_rad_per_second"),
             py::arg("polar_motion_y_rad_per_second"), py::arg("celestial_pole_dx_rad_per_second"),
             py::arg("celestial_pole_dy_rad_per_second"))
        .def_readonly("ut1_minus_tai_seconds_per_second",
                      &core::EarthOrientationRates::ut1_minus_tai_seconds_per_second)
        .def_readonly("polar_motion_x_rad_per_second",
                      &core::EarthOrientationRates::polar_motion_x_rad_per_second)
        .def_readonly("polar_motion_y_rad_per_second",
                      &core::EarthOrientationRates::polar_motion_y_rad_per_second)
        .def_readonly("celestial_pole_dx_rad_per_second",
                      &core::EarthOrientationRates::celestial_pole_dx_rad_per_second)
        .def_readonly("celestial_pole_dy_rad_per_second",
                      &core::EarthOrientationRates::celestial_pole_dy_rad_per_second);
    py::class_<core::EarthOrientationState>(module, "EarthOrientationState")
        .def(py::init<core::EarthOrientationSample, core::EarthOrientationRates>(),
             py::arg("sample"), py::arg("rates"))
        .def_readonly("sample", &core::EarthOrientationState::sample)
        .def_readonly("rates", &core::EarthOrientationState::rates);
    py::class_<core::EopDataInfo>(module, "EopDataInfo")
        .def_readonly("id", &core::EopDataInfo::id)
        .def_readonly("sha256", &core::EopDataInfo::sha256)
        .def_readonly("source_url", &core::EopDataInfo::source_url)
        .def_readonly("reference_frame", &core::EopDataInfo::reference_frame)
        .def_readonly("interpolation", &core::EopDataInfo::interpolation)
        .def_readonly("first_epoch", &core::EopDataInfo::first_epoch)
        .def_readonly("last_epoch", &core::EopDataInfo::last_epoch)
        .def_readonly("time_data", &core::EopDataInfo::time_data)
        .def_readonly("extrapolation_days", &core::EopDataInfo::extrapolation_days);
    py::class_<core::IEarthOrientationProvider>(module, "EarthOrientationProvider")
        .def("at", &core::IEarthOrientationProvider::at, py::arg("epoch"),
             py::call_guard<py::gil_scoped_release>())
        .def_property_readonly("data_info", &core::IEarthOrientationProvider::data_info,
                               py::return_value_policy::copy);
    py::class_<core::PositionRotation>(module, "PositionRotation")
        .def_readonly("epoch", &core::PositionRotation::epoch)
        .def_readonly("origin", &core::PositionRotation::origin)
        .def_readonly("from_frame", &core::PositionRotation::from)
        .def_readonly("to_frame", &core::PositionRotation::to)
        .def_readonly("matrix", &core::PositionRotation::matrix)
        .def_readonly("model", &core::PositionRotation::model)
        .def_readonly("eop_source_id", &core::PositionRotation::eop_source_id)
        .def("apply_position", &core::PositionRotation::apply_position, py::arg("position_m"))
        .def("inverse_position", &core::PositionRotation::inverse_position, py::arg("position_m"));
    py::class_<core::IFrameProvider>(module, "FrameProvider")
        .def("gcrf_to_itrf", &core::IFrameProvider::gcrf_to_itrf, py::arg("epoch"), py::arg("eop"),
             py::call_guard<py::gil_scoped_release>())
        .def("gcrf_to_itrf_state", &core::IFrameProvider::gcrf_to_itrf_state, py::arg("epoch"),
             py::arg("eop"), py::call_guard<py::gil_scoped_release>());
    py::class_<core::StateTransform>(module, "StateTransform")
        .def_readonly("rotation", &core::StateTransform::rotation)
        .def_readonly("rotation_rate_per_second", &core::StateTransform::rotation_rate_per_second)
        .def_readonly("derivative_model", &core::StateTransform::derivative_model)
        .def("apply", &core::StateTransform::apply, py::arg("state"))
        .def("inverse", &core::StateTransform::inverse, py::arg("state"));
    module.def("time_frames_available", &runtime::time_frames_available);
    module.def("make_builtin_time_provider", &runtime::make_builtin_time_provider);
    module.def("load_time_snapshot", &runtime::load_time_snapshot, py::arg("file"),
               py::call_guard<py::gil_scoped_release>());
    module.def("make_frame_provider", &runtime::make_frame_provider);
    module.def("load_eop_snapshot", &runtime::load_eop_snapshot, py::arg("manifest"),
               py::arg("extrapolation_days") = 0, py::call_guard<py::gil_scoped_release>());
    module.def("import_eop_snapshot", &runtime::import_eop_snapshot, py::arg("manifest"),
               py::arg("store"), py::call_guard<py::gil_scoped_release>());
}
