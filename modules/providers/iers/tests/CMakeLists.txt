add_executable(orbitforge_iers_tests eop_state_test.cpp)
target_link_libraries(orbitforge_iers_tests PRIVATE OrbitForge::Iers OrbitForge::Erfa
    GTest::gtest_main nlohmann_json::nlohmann_json)
target_include_directories(orbitforge_iers_tests SYSTEM PRIVATE "${PICOSHA2_INCLUDE_DIR}")
target_compile_definitions(orbitforge_iers_tests PRIVATE
    ORBITFORGE_EOP_SAMPLE="${PROJECT_SOURCE_DIR}/data/samples/eop/iers_c04_2016_12_28_2017_01_04/manifest.json"
    ORBITFORGE_EOP_TEST_DIR="${CMAKE_CURRENT_BINARY_DIR}/fixtures")
orbitforge_warnings(orbitforge_iers_tests)
orbitforge_copy_runtime_dlls(orbitforge_iers_tests)
gtest_discover_tests(orbitforge_iers_tests DISCOVERY_MODE PRE_TEST TEST_PREFIX "iers.")
