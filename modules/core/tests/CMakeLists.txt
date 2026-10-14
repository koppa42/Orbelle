add_executable(orbitforge_core_tests circular_two_body_test.cpp ground_station_test.cpp hpop_test.cpp mission_test.cpp)
target_compile_definitions(orbitforge_core_tests PRIVATE
    ORBITFORGE_HPOP_REFERENCE="${PROJECT_SOURCE_DIR}/tests/reference/propagators/hpop.txt")
target_link_libraries(orbitforge_core_tests PRIVATE OrbitForge::Core GTest::gtest_main)
orbitforge_warnings(orbitforge_core_tests)
orbitforge_copy_runtime_dlls(orbitforge_core_tests)
gtest_discover_tests(orbitforge_core_tests DISCOVERY_MODE PRE_TEST TEST_PREFIX "core.")

