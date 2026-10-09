add_executable(orbitforge_spice_tests provider_test.cpp)
target_link_libraries(orbitforge_spice_tests PRIVATE OrbitForge::Spice GTest::gtest_main
    unofficial::cspice::cspice nlohmann_json::nlohmann_json)
target_include_directories(orbitforge_spice_tests SYSTEM PRIVATE "${PICOSHA2_INCLUDE_DIR}")
target_compile_definitions(orbitforge_spice_tests PRIVATE
    ORBITFORGE_SPICE_SAMPLE="${PROJECT_SOURCE_DIR}/data/samples/spice/de440s_2016_leap/manifest.json"
    ORBITFORGE_SPICE_TEST_DIR="${CMAKE_CURRENT_BINARY_DIR}/fixtures")
if(ORBITFORGE_WITH_ERFA)
    find_package(erfa 2.0.1 EXACT CONFIG REQUIRED)
    target_link_libraries(orbitforge_spice_tests PRIVATE OrbitForge::Erfa erfa::erfa)
    target_compile_definitions(orbitforge_spice_tests PRIVATE ORBITFORGE_SPICE_TEST_ERFA)
endif()
orbitforge_warnings(orbitforge_spice_tests)
orbitforge_copy_runtime_dlls(orbitforge_spice_tests)
gtest_discover_tests(orbitforge_spice_tests TEST_PREFIX "spice.")
