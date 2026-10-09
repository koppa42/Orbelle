add_executable(orbitforge_integration_tests runtime_test.cpp)
target_link_libraries(orbitforge_integration_tests PRIVATE OrbitForge::Runtime GTest::gtest_main)
target_compile_definitions(orbitforge_integration_tests PRIVATE
    ORBITFORGE_EXPECT_ERFA=$<BOOL:${ORBITFORGE_WITH_ERFA}>
    ORBITFORGE_EXPECT_SPICE=$<BOOL:${ORBITFORGE_WITH_SPICE}>
    ORBITFORGE_INTEGRATION_SPICE="${PROJECT_SOURCE_DIR}/data/samples/spice/de440s_2016_leap/manifest.json"
    ORBITFORGE_INTEGRATION_WORKSPACE="${CMAKE_CURRENT_BINARY_DIR}/spice-workspace")
if(ORBITFORGE_BUILD_VISUALIZATION)
    target_link_libraries(orbitforge_integration_tests PRIVATE OrbitForge::Visualization)
    target_compile_definitions(orbitforge_integration_tests PRIVATE ORBITFORGE_HAS_VISUALIZATION)
endif()
orbitforge_warnings(orbitforge_integration_tests)
orbitforge_copy_runtime_dlls(orbitforge_integration_tests)
gtest_discover_tests(orbitforge_integration_tests DISCOVERY_MODE PRE_TEST TEST_PREFIX "integration.")

