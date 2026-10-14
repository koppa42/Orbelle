#include <orbitforge/runtime/data_catalog.hpp>
#include <pybind11/pybind11.h>
#include <pybind11/stl.h>
#include <pybind11/stl/filesystem.h>
namespace py = pybind11;
using namespace orbitforge;
void bind_data_catalog(py::module_ &module) {
    py::class_<core::ThirdBodyDataInfo>(module, "ThirdBodyDataInfo")
        .def_readonly("model", &core::ThirdBodyDataInfo::model)
        .def_readonly("content_sha256", &core::ThirdBodyDataInfo::content_sha256)
        .def_readonly("interpretation_version", &core::ThirdBodyDataInfo::interpretation_version)
        .def_readonly("first", &core::ThirdBodyDataInfo::first)
        .def_readonly("last", &core::ThirdBodyDataInfo::last)
        .def_readonly("sun_mu", &core::ThirdBodyDataInfo::sun_mu)
        .def_readonly("moon_mu", &core::ThirdBodyDataInfo::moon_mu);
    py::class_<runtime::CatalogEphemeris>(module, "CatalogEphemeris")
        .def_readonly("selection", &runtime::CatalogEphemeris::selection)
        .def_readonly("info", &runtime::CatalogEphemeris::info)
        .def_readonly("manifest", &runtime::CatalogEphemeris::manifest)
        .def_readonly("acquisition", &runtime::CatalogEphemeris::acquisition)
        .def_readonly("acquired_at_unix_seconds",
                      &runtime::CatalogEphemeris::acquired_at_unix_seconds);
    py::class_<core::GravityInfo>(module, "GravityInfo")
        .def_readonly("model", &core::GravityInfo::model)
        .def_readonly("sha256", &core::GravityInfo::sha256)
        .def_readonly("source_url", &core::GravityInfo::source_url)
        .def_readonly("tide_system", &core::GravityInfo::tide_system)
        .def_readonly("mu", &core::GravityInfo::mu)
        .def_readonly("radius_m", &core::GravityInfo::radius_m)
        .def_readonly("degree", &core::GravityInfo::degree)
        .def_readonly("order", &core::GravityInfo::order);
    py::class_<runtime::CatalogGravity>(module, "CatalogGravity")
        .def_readonly("selection", &runtime::CatalogGravity::selection)
        .def_readonly("info", &runtime::CatalogGravity::info)
        .def_readonly("file", &runtime::CatalogGravity::file)
        .def_readonly("acquisition", &runtime::CatalogGravity::acquisition)
        .def_readonly("acquired_at_unix_seconds",
                      &runtime::CatalogGravity::acquired_at_unix_seconds);
    module.def("data_catalog_available", &runtime::data_catalog_available);
    py::enum_<runtime::EopProduct>(module, "EopProduct")
        .value("C04", runtime::EopProduct::c04)
        .value("FINALS2000A", runtime::EopProduct::finals2000a);
    py::enum_<runtime::DataAcquisition>(module, "DataAcquisition")
        .value("unknown", runtime::DataAcquisition::unknown)
        .value("bundled", runtime::DataAcquisition::bundled)
        .value("imported", runtime::DataAcquisition::imported)
        .value("downloaded", runtime::DataAcquisition::downloaded);
    py::class_<runtime::CatalogSnapshot>(module, "CatalogSnapshot")
        .def_readonly("lock_sha256", &runtime::CatalogSnapshot::lock_sha256)
        .def_readonly("snapshot_id", &runtime::CatalogSnapshot::snapshot_id)
        .def_readonly("source_url", &runtime::CatalogSnapshot::source_url)
        .def_readonly("first", &runtime::CatalogSnapshot::first)
        .def_readonly("last", &runtime::CatalogSnapshot::last)
        .def_readonly("manifest", &runtime::CatalogSnapshot::manifest)
        .def_readonly("acquisition", &runtime::CatalogSnapshot::acquisition)
        .def_readonly("acquired_at_unix_seconds",
                      &runtime::CatalogSnapshot::acquired_at_unix_seconds);
    py::class_<runtime::DataCatalog>(module, "DataCatalog")
        .def(py::init<std::filesystem::path>(), py::arg("root"))
        .def_property_readonly("root", &runtime::DataCatalog::root)
        .def_property_readonly("gravity_models", &runtime::DataCatalog::gravity_models)
        .def_property_readonly("ephemerides", &runtime::DataCatalog::ephemerides)
        .def(
            "install_de440s",
            [](runtime::DataCatalog &catalog, const std::filesystem::path &file) {
                return catalog.install_de440s(file);
            },
            py::arg("file"), py::call_guard<py::gil_scoped_release>())
        .def(
            "install_gravity",
            [](runtime::DataCatalog &catalog, const std::filesystem::path &file,
               const std::string &model,
               const std::string &url) { return catalog.install_gravity(file, model, url); },
            py::arg("file"), py::arg("model") = "custom", py::arg("source_url") = "",
            py::call_guard<py::gil_scoped_release>())
        .def("snapshot", &runtime::DataCatalog::snapshot, py::arg("first"), py::arg("last"),
             py::kw_only(), py::arg("earth_orientation") = true,
             py::call_guard<py::gil_scoped_release>())
        .def_property("preferred_eop", &runtime::DataCatalog::preferred_eop,
                      &runtime::DataCatalog::set_preferred_eop)
        .def("extrapolation_days", &runtime::DataCatalog::extrapolation_days, py::arg("product"))
        .def("set_extrapolation_days", &runtime::DataCatalog::set_extrapolation_days,
             py::arg("product"), py::arg("days"))
        .def("activate_eop", &runtime::DataCatalog::activate_eop, py::arg("lock_sha256"),
             py::call_guard<py::gil_scoped_release>())
        .def_property_readonly("snapshots", &runtime::DataCatalog::snapshots)
        .def("record_acquisition", &runtime::DataCatalog::record_acquisition,
             py::arg("lock_sha256"), py::arg("acquisition"), py::arg("acquired_at") = std::nullopt)
        .def("install_eop", &runtime::DataCatalog::install_eop, py::arg("manifest"),
             py::arg("time_file") = "", py::call_guard<py::gil_scoped_release>())
        .def("import_finals", &runtime::DataCatalog::import_finals, py::arg("source"),
             py::arg("source_url"), py::arg("retrieved_on"), py::arg("time_file") = "",
             py::call_guard<py::gil_scoped_release>())
        .def("import_c04", &runtime::DataCatalog::import_c04, py::arg("source"),
             py::arg("source_url"), py::arg("retrieved_on"), py::arg("time_file") = "",
             py::call_guard<py::gil_scoped_release>())
        .def("install_time", &runtime::DataCatalog::install_time, py::arg("source"))
        .def("time_file", &runtime::DataCatalog::time_file, py::arg("sha256"))
        .def("resolve_eop", &runtime::DataCatalog::resolve_eop, py::arg("data_lock"),
             py::call_guard<py::gil_scoped_release>())
        .def("load_eop", &runtime::DataCatalog::load_eop, py::arg("lock_sha256"),
             py::call_guard<py::gil_scoped_release>())
        .def("setting", &runtime::DataCatalog::setting)
        .def("set_setting", &runtime::DataCatalog::set_setting)
        .def("set_settings", &runtime::DataCatalog::set_settings, py::arg("values"));
}
