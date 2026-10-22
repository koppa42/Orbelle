add_executable(orbelle_erfa_tests time_frames_test.cpp teme_test.cpp celestial_test.cpp)
target_link_libraries(orbelle_erfa_tests PRIVATE Orbelle::Erfa GTest::gtest_main erfa::erfa)
target_compile_definitions(orbelle_erfa_tests PRIVATE
    ORBELLE_REFERENCE_DIR="${PROJECT_SOURCE_DIR}/tests/reference/time_frames")
orbelle_warnings(orbelle_erfa_tests)
orbelle_copy_runtime_dlls(orbelle_erfa_tests)
gtest_discover_tests(orbelle_erfa_tests DISCOVERY_MODE PRE_TEST TEST_PREFIX "erfa.")
