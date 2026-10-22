add_executable(orbelle_iers_tests eop_state_test.cpp)
target_link_libraries(orbelle_iers_tests PRIVATE Orbelle::Iers Orbelle::Erfa
    GTest::gtest_main nlohmann_json::nlohmann_json)
target_include_directories(orbelle_iers_tests SYSTEM PRIVATE "${PICOSHA2_INCLUDE_DIR}")
target_compile_definitions(orbelle_iers_tests PRIVATE
    ORBELLE_FINALS_SAMPLE="${PROJECT_SOURCE_DIR}/data/samples/eop/iers_finals2000a_2026_09_01_2026_11_01/manifest.json"
    ORBELLE_EOP_SAMPLE="${PROJECT_SOURCE_DIR}/data/samples/eop/iers_c04_2016_12_28_2017_01_04/manifest.json"
    ORBELLE_EOP_TEST_DIR="${PROJECT_BINARY_DIR}/fixtures/iers")
orbelle_warnings(orbelle_iers_tests)
orbelle_copy_runtime_dlls(orbelle_iers_tests)
gtest_discover_tests(orbelle_iers_tests DISCOVERY_MODE PRE_TEST TEST_PREFIX "iers.")
