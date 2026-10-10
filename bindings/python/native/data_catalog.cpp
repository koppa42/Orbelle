#include <orbitforge/runtime/data_catalog.hpp>
#include <pybind11/pybind11.h>
#include <pybind11/stl.h>
#include <pybind11/stl/filesystem.h>
namespace py = pybind11;
using namespace orbitforge;
void bind_data_catalog(py::module_ &module) {
    module.def("data_catalog_available", &runtime::data_catalog_available);
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
        .def("set_setting", &runtime::DataCatalog::set_setting);
}
