find_package(Python REQUIRED COMPONENTS Interpreter Development.Module)
set(PYBIND11_FINDPYTHON ON)
find_package(pybind11 3.0 CONFIG REQUIRED)
pybind11_add_module(_native MODULE native/module.cpp native/time_frames.cpp native/ephemeris.cpp)
target_link_libraries(_native PRIVATE OrbitForge::Runtime)
target_compile_definitions(_native PRIVATE ORBITFORGE_VERSION="${PROJECT_VERSION}")
orbitforge_warnings(_native)
install(TARGETS _native
    LIBRARY DESTINATION orbitforge COMPONENT Python
    RUNTIME DESTINATION orbitforge COMPONENT Python)

