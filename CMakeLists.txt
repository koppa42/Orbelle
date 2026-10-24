cmake_minimum_required(VERSION 3.28)
project(Orbelle VERSION 0.4.0 LANGUAGES CXX)

include(cmake/OrbelleOptions.cmake)
include(cmake/OrbelleWarnings.cmake)
include(cmake/OrbelleSanitizers.cmake)
include(cmake/OrbelleModule.cmake)
include(GNUInstallDirs)
include(CTest)

set(CMAKE_CXX_EXTENSIONS OFF)
# C++20 is used without named modules; avoid a separate dependency scan per source.
set(CMAKE_CXX_SCAN_FOR_MODULES OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

if(BUILD_TESTING)
    find_package(GTest CONFIG REQUIRED)
    include(GoogleTest)
endif()

add_subdirectory(modules/core)
add_subdirectory(modules/providers/sgp4)
if(ORBELLE_WITH_ERFA)
    add_subdirectory(modules/providers/erfa)
    add_subdirectory(modules/providers/iers)
endif()
if(ORBELLE_WITH_SPICE)
    add_subdirectory(modules/providers/spice)
endif()
if(ORBELLE_BUILD_APPLICATION)
    add_subdirectory(modules/results)
    add_subdirectory(modules/application)
    add_subdirectory(modules/runtime)
endif()
if(ORBELLE_BUILD_VISUALIZATION)
    add_subdirectory(modules/visualization)
endif()
if(ORBELLE_WITH_VSG)
    add_subdirectory(modules/render_vsg)
endif()
if(ORBELLE_WITH_ROCKY)
    add_subdirectory(modules/geo_rocky)
endif()
if(ORBELLE_BUILD_EXAMPLES)
    add_subdirectory(examples/cpp)
endif()
if(ORBELLE_BUILD_PYTHON)
    add_subdirectory(bindings/python)
endif()
if(ORBELLE_BUILD_COMPUTE_BENCHMARKS)
    add_subdirectory(benchmarks/compute)
endif()
if(ORBELLE_BUILD_DESKTOP)
    if(ORBELLE_BUILD_BENCHMARKS)
        add_subdirectory(benchmarks/graphics)
    endif()
    add_subdirectory(apps/desktop)
endif()
if(BUILD_TESTING AND ORBELLE_BUILD_APPLICATION)
    add_subdirectory(tests/integration)
endif()

if(ORBELLE_INSTALL_SDK)
    include(CMakePackageConfigHelpers)
    configure_package_config_file(
        cmake/OrbelleConfig.cmake.in
        "${CMAKE_CURRENT_BINARY_DIR}/OrbelleConfig.cmake"
        INSTALL_DESTINATION "${CMAKE_INSTALL_LIBDIR}/cmake/Orbelle")
    write_basic_package_version_file(
        "${CMAKE_CURRENT_BINARY_DIR}/OrbelleConfigVersion.cmake"
        VERSION "${PROJECT_VERSION}" COMPATIBILITY ExactVersion)
    install(EXPORT OrbelleTargets NAMESPACE Orbelle::
        DESTINATION "${CMAKE_INSTALL_LIBDIR}/cmake/Orbelle" COMPONENT SDK)
    install(FILES
        "${CMAKE_CURRENT_BINARY_DIR}/OrbelleConfig.cmake"
        "${CMAKE_CURRENT_BINARY_DIR}/OrbelleConfigVersion.cmake"
        DESTINATION "${CMAKE_INSTALL_LIBDIR}/cmake/Orbelle" COMPONENT SDK)
endif()

