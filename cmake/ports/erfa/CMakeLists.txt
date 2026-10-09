cmake_minimum_required(VERSION 3.28)
project(erfa VERSION 2.0.1 LANGUAGES C)
include(GNUInstallDirs)
include(CMakePackageConfigHelpers)
option(BUILD_TESTING "Run the unmodified upstream ERFA reference suite" OFF)

# This wrapper only supplies a CMake build; the pinned upstream C sources are unmodified.
file(GLOB sources CONFIGURE_DEPENDS src/*.c)
list(FILTER sources EXCLUDE REGEX "/t_erfa_c(_extra)?\\.c$")
add_library(erfa STATIC ${sources})
add_library(erfa::erfa ALIAS erfa)
set_target_properties(erfa PROPERTIES POSITION_INDEPENDENT_CODE ON)
target_include_directories(erfa PUBLIC
    "$<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/src>"
    "$<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>")
target_compile_definitions(erfa PRIVATE
    PACKAGE_VERSION="2.0.1" PACKAGE_VERSION_MAJOR=2 PACKAGE_VERSION_MINOR=0
    PACKAGE_VERSION_MICRO=1 SOFA_VERSION="20231011")
if(NOT WIN32)
    target_link_libraries(erfa PUBLIC m)
endif()
if(BUILD_TESTING)
    enable_testing()
    foreach(test t_erfa_c t_erfa_c_extra)
        add_executable(${test} src/${test}.c)
        target_link_libraries(${test} PRIVATE erfa)
        add_test(NAME ${test} COMMAND ${test})
    endforeach()
endif()
install(TARGETS erfa EXPORT erfaTargets ARCHIVE DESTINATION "${CMAKE_INSTALL_LIBDIR}")
install(FILES src/erfa.h src/erfam.h src/erfaextra.h src/erfadatextra.h
    DESTINATION "${CMAKE_INSTALL_INCLUDEDIR}")
install(EXPORT erfaTargets NAMESPACE erfa:: DESTINATION "${CMAKE_INSTALL_LIBDIR}/cmake/erfa")
configure_package_config_file(erfaConfig.cmake.in "${CMAKE_CURRENT_BINARY_DIR}/erfaConfig.cmake"
    INSTALL_DESTINATION "${CMAKE_INSTALL_LIBDIR}/cmake/erfa")
write_basic_package_version_file("${CMAKE_CURRENT_BINARY_DIR}/erfaConfigVersion.cmake"
    VERSION "${PROJECT_VERSION}" COMPATIBILITY SameMajorVersion)
install(FILES "${CMAKE_CURRENT_BINARY_DIR}/erfaConfig.cmake"
    "${CMAKE_CURRENT_BINARY_DIR}/erfaConfigVersion.cmake"
    DESTINATION "${CMAKE_INSTALL_LIBDIR}/cmake/erfa")
