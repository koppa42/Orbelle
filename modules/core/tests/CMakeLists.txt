add_executable(orbitforge_core_tests circular_two_body_test.cpp ground_station_test.cpp hpop_test.cpp mission_test.cpp gravity_test.cpp solid_tides_test.cpp third_body_test.cpp solar_radiation_pressure_test.cpp atmospheric_drag_test.cpp)
target_compile_definitions(orbitforge_core_tests PRIVATE
    ORBITFORGE_SOLID_TIDE_REFERENCE="${PROJECT_SOURCE_DIR}/tests/reference/propagators/solid_tides.txt"
    ORBITFORGE_GRAVITY_REFERENCE="${PROJECT_SOURCE_DIR}/tests/reference/propagators/gravity.txt"
    ORBITFORGE_THIRD_BODY_REFERENCE="${PROJECT_SOURCE_DIR}/tests/reference/propagators/third_body.txt"
    ORBITFORGE_SRP_REFERENCE="${PROJECT_SOURCE_DIR}/tests/reference/propagators/srp.txt"
    ORBITFORGE_SRP_GEOMETRY_REFERENCE="${PROJECT_SOURCE_DIR}/tests/reference/propagators/srp_geometry.txt"
    ORBITFORGE_NRLMSISE_AP_REFERENCE="${PROJECT_SOURCE_DIR}/tests/reference/propagators/nrlmsise00_ap.txt"
    ORBITFORGE_NRLMSISE_REFERENCE="${PROJECT_SOURCE_DIR}/tests/reference/propagators/nrlmsise00.txt"
    ORBITFORGE_JR_REFERENCE="${PROJECT_SOURCE_DIR}/tests/reference/propagators/jacchia_roberts.txt"
    ORBITFORGE_DRAG_REFERENCE="${PROJECT_SOURCE_DIR}/tests/reference/propagators/drag.txt"
    ORBITFORGE_HPOP_REFERENCE="${PROJECT_SOURCE_DIR}/tests/reference/propagators/hpop.txt")
target_link_libraries(orbitforge_core_tests PRIVATE OrbitForge::Core GTest::gtest_main)
orbitforge_warnings(orbitforge_core_tests)
orbitforge_copy_runtime_dlls(orbitforge_core_tests)
gtest_discover_tests(orbitforge_core_tests DISCOVERY_MODE PRE_TEST TEST_PREFIX "core.")

