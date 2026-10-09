cmake_minimum_required(VERSION 3.28)
project(OrbitForge VERSION 0.1.0 LANGUAGES CXX)

include(cmake/OrbitForgeOptions.cmake)
include(cmake/OrbitForgeWarnings.cmake)
include(cmake/OrbitForgeSanitizers.cmake)
include(cmake/OrbitForgeModule.cmake)
include(GNUInstallDirs)
include(CTest)

set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

if(BUILD_TESTING)
    find_package(GTest CONFIG REQUIRED)
    include(GoogleTest)
endif()

add_subdirectory(modules/core)
if(ORBITFORGE_WITH_ERFA)
    add_subdirectory(modules/providers/erfa)
    add_subdirectory(modules/providers/iers)
endif()
if(ORBITFORGE_WITH_SPICE)
    add_subdirectory(modules/providers/spice)
endif()
if(ORBITFORGE_BUILD_APPLICATION)
    add_subdirectory(modules/results)
    add_subdirectory(modules/application)
    add_subdirectory(modules/runtime)
endif()
if(ORBITFORGE_BUILD_VISUALIZATION)
    add_subdirectory(modules/visualization)
endif()
if(ORBITFORGE_WITH_VSG)
    add_subdirectory(modules/render_vsg)
endif()
if(ORBITFORGE_WITH_ROCKY)
    add_subdirectory(modules/geo_rocky)
endif()
if(ORBITFORGE_BUILD_EXAMPLES)
    add_subdirectory(examples/cpp)
endif()
if(ORBITFORGE_BUILD_PYTHON)
    add_subdirectory(bindings/python)
endif()
if(ORBITFORGE_BUILD_DESKTOP)
    if(ORBITFORGE_BUILD_BENCHMARKS)
        add_subdirectory(benchmarks/graphics)
    endif()
    add_subdirectory(apps/desktop)
endif()
if(BUILD_TESTING AND ORBITFORGE_BUILD_APPLICATION)
    add_subdirectory(tests/integration)
endif()

if(ORBITFORGE_INSTALL_SDK)
    include(CMakePackageConfigHelpers)
    configure_package_config_file(
        cmake/OrbitForgeConfig.cmake.in
        "${CMAKE_CURRENT_BINARY_DIR}/OrbitForgeConfig.cmake"
        INSTALL_DESTINATION "${CMAKE_INSTALL_LIBDIR}/cmake/OrbitForge")
    write_basic_package_version_file(
        "${CMAKE_CURRENT_BINARY_DIR}/OrbitForgeConfigVersion.cmake"
        VERSION "${PROJECT_VERSION}" COMPATIBILITY ExactVersion)
    install(EXPORT OrbitForgeTargets NAMESPACE OrbitForge::
        DESTINATION "${CMAKE_INSTALL_LIBDIR}/cmake/OrbitForge" COMPONENT SDK)
    install(FILES
        "${CMAKE_CURRENT_BINARY_DIR}/OrbitForgeConfig.cmake"
        "${CMAKE_CURRENT_BINARY_DIR}/OrbitForgeConfigVersion.cmake"
        DESTINATION "${CMAKE_INSTALL_LIBDIR}/cmake/OrbitForge" COMPONENT SDK)
endif()

