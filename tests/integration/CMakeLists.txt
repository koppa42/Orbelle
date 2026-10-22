add_executable(orbelle_integration_tests runtime_test.cpp hpop_covariance_project_test.cpp tabulated_orbit_test.cpp hpop_ephemeris_test.cpp scenario_test.cpp project_test.cpp report_test.cpp
    propagators_test.cpp access_test.cpp lighting_test.cpp orbital_events_test.cpp intersatellite_test.cpp coverage_test.cpp project_document_test.cpp analysis_test.cpp orbit_session_cache_test.cpp catalog_analysis_test.cpp
    project_analysis_persistence_test.cpp analysis_access_test.cpp project_facade_test.cpp hpop_test.cpp mission_test.cpp gravity_test.cpp third_body_test.cpp planetary_centres_test.cpp solar_radiation_pressure_test.cpp atmospheric_drag_test.cpp space_weather_test.cpp mean_weather_test.cpp monthly_weather_test.cpp fxm_weather_test.cpp ocean_tides_test.cpp secular_gravity_test.cpp)
target_link_libraries(orbelle_integration_tests PRIVATE Orbelle::Runtime GTest::gtest_main)
target_compile_definitions(orbelle_integration_tests PRIVATE
    ORBELLE_WEATHER_MONTHLY_DIR="${PROJECT_SOURCE_DIR}/tests/reference/propagators"
    ORBELLE_WEATHER_FXM_DIR="${PROJECT_SOURCE_DIR}/tests/reference/propagators"
    ORBELLE_WEATHER_MEAN_REFERENCE="${PROJECT_SOURCE_DIR}/tests/reference/propagators/space_weather_mean.txt"
    ORBELLE_WEATHER_SPLINE_REFERENCE="${PROJECT_SOURCE_DIR}/tests/reference/propagators/space_weather_spline.txt"
    ORBELLE_SECULAR_REFERENCE="${PROJECT_SOURCE_DIR}/tests/reference/propagators"
    ORBELLE_OCEAN_REFERENCE="${PROJECT_SOURCE_DIR}/tests/reference/propagators"
    ORBELLE_EGM2008_FULL_REFERENCE="${PROJECT_SOURCE_DIR}/tests/reference/propagators/gravity_egm2008_full.txt"
    ORBELLE_COVERAGE_REFERENCE="${PROJECT_SOURCE_DIR}/tests/reference/propagators/coverage_orekit.txt"
    ORBELLE_INTERSATELLITE_REFERENCE="${PROJECT_SOURCE_DIR}/tests/reference/propagators/intersatellite_orekit.txt"
    ORBELLE_ORBITAL_EVENTS_REFERENCE="${PROJECT_SOURCE_DIR}/tests/reference/propagators/orbital_events_orekit.txt"
    ORBELLE_LIGHTING_REFERENCE="${PROJECT_SOURCE_DIR}/tests/reference/propagators/lighting_orekit.txt"
    ORBELLE_ACCESS_REFERENCE="${PROJECT_SOURCE_DIR}/tests/reference/propagators/access_orekit.txt"
    ORBELLE_REPORT_EOP="${PROJECT_SOURCE_DIR}/data/samples/eop/iers_c04_2000_01_01_2000_01_02/manifest.json"
    ORBELLE_DRAG_EOP="${PROJECT_SOURCE_DIR}/data/samples/eop/iers_c04_2016_12_28_2017_01_04/manifest.json"
    ORBELLE_EXPECT_ERFA=$<BOOL:${ORBELLE_WITH_ERFA}>
    ORBELLE_EXPECT_SPICE=$<BOOL:${ORBELLE_WITH_SPICE}>
    ORBELLE_PROJECT_CURRENT="${CMAKE_CURRENT_SOURCE_DIR}/fixtures/project-current.orbelle"
    ORBELLE_INTEGRATION_SPICE="${PROJECT_SOURCE_DIR}/data/samples/spice/de440s_2016_leap/manifest.json"
    ORBELLE_INTEGRATION_PLANETS="${PROJECT_SOURCE_DIR}/data/samples/spice/de440s_planets_2016_leap/manifest.json"
    ORBELLE_INTEGRATION_WORKSPACE="${CMAKE_CURRENT_BINARY_DIR}/spice-workspace")
if(ORBELLE_BUILD_VISUALIZATION)
    target_link_libraries(orbelle_integration_tests PRIVATE Orbelle::Visualization)
    target_compile_definitions(orbelle_integration_tests PRIVATE ORBELLE_HAS_VISUALIZATION)
endif()
orbelle_warnings(orbelle_integration_tests)
orbelle_copy_runtime_dlls(orbelle_integration_tests)
gtest_discover_tests(orbelle_integration_tests DISCOVERY_MODE PRE_TEST
    NO_PRETTY_VALUES TEST_PREFIX "integration.")

target_include_directories(orbelle_integration_tests PRIVATE "${PROJECT_SOURCE_DIR}/tests/support")

target_sources(orbelle_integration_tests PRIVATE sensor_view_test.cpp)
