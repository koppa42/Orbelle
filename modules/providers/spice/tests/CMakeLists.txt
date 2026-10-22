add_executable(orbelle_spice_tests provider_test.cpp two_body_test.cpp j2_test.cpp j4_test.cpp)
target_link_libraries(orbelle_spice_tests PRIVATE Orbelle::Spice GTest::gtest_main
    unofficial::cspice::cspice nlohmann_json::nlohmann_json)
target_include_directories(orbelle_spice_tests SYSTEM PRIVATE "${PICOSHA2_INCLUDE_DIR}")
target_compile_definitions(orbelle_spice_tests PRIVATE
    ORBELLE_J4_REFERENCE="${PROJECT_SOURCE_DIR}/tests/reference/propagators/j4.txt"
    ORBELLE_SPICE_SAMPLE="${PROJECT_SOURCE_DIR}/data/samples/spice/de440s_2016_leap/manifest.json"
    ORBELLE_SPICE_TEST_DIR="${CMAKE_CURRENT_BINARY_DIR}/fixtures")
if(ORBELLE_WITH_ERFA)
    find_package(erfa 2.0.1 EXACT CONFIG REQUIRED)
    target_link_libraries(orbelle_spice_tests PRIVATE Orbelle::Erfa erfa::erfa)
    target_compile_definitions(orbelle_spice_tests PRIVATE ORBELLE_SPICE_TEST_ERFA)
endif()
orbelle_warnings(orbelle_spice_tests)
orbelle_copy_runtime_dlls(orbelle_spice_tests)
gtest_discover_tests(orbelle_spice_tests TEST_PREFIX "spice.")
