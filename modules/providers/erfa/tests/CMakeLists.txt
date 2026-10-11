add_executable(orbitforge_erfa_tests time_frames_test.cpp teme_test.cpp)
target_link_libraries(orbitforge_erfa_tests PRIVATE OrbitForge::Erfa GTest::gtest_main)
target_compile_definitions(orbitforge_erfa_tests PRIVATE
    ORBITFORGE_REFERENCE_DIR="${PROJECT_SOURCE_DIR}/tests/reference/time_frames")
orbitforge_warnings(orbitforge_erfa_tests)
orbitforge_copy_runtime_dlls(orbitforge_erfa_tests)
gtest_discover_tests(orbitforge_erfa_tests DISCOVERY_MODE PRE_TEST TEST_PREFIX "erfa.")
