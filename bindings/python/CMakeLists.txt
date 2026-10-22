find_package(Python REQUIRED COMPONENTS Interpreter Development.Module)
set(PYBIND11_FINDPYTHON ON)
find_package(pybind11 3.0 CONFIG REQUIRED)
pybind11_add_module(_native MODULE native/module.cpp native/time_frames.cpp native/ephemeris.cpp native/j2.cpp native/orbit_design.cpp native/project.cpp native/project_document.cpp native/data_catalog.cpp native/report.cpp native/analysis.cpp)
target_link_libraries(_native PRIVATE Orbelle::Runtime)
target_compile_definitions(_native PRIVATE ORBELLE_VERSION="${PROJECT_VERSION}")
orbelle_warnings(_native)
install(TARGETS _native
    LIBRARY DESTINATION orbelle COMPONENT Python
    RUNTIME DESTINATION orbelle COMPONENT Python)
if(WIN32 AND ORBELLE_WITH_DATA_CATALOG)
    install(FILES $<TARGET_RUNTIME_DLLS:_native> DESTINATION orbelle COMPONENT Python)
endif()

