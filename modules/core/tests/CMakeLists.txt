add_executable(orbelle_core_tests universal_kepler_test.cpp vop_interpolation_test.cpp circular_two_body_test.cpp tabulated_orbit_test.cpp ground_station_test.cpp hpop_test.cpp hpop_force_test.cpp hpop_variational_test.cpp hpop_covariance_test.cpp hpop_covariance_frames_test.cpp sundman_test.cpp mission_test.cpp srp_differential_test.cpp drag_differential_test.cpp force_differential_test.cpp gravity_differential_test.cpp gravity_test.cpp solid_tides_test.cpp ocean_tides_test.cpp relativity_test.cpp third_body_test.cpp solar_radiation_pressure_test.cpp atmospheric_drag_test.cpp)
target_sources(orbelle_core_tests PRIVATE region_test.cpp)
target_compile_definitions(orbelle_core_tests PRIVATE
    ORBELLE_ORBIT_DESIGN_REFERENCE="${PROJECT_SOURCE_DIR}/tests/reference/propagators/orbit_design.txt"
    ORBELLE_HPOP_COVARIANCE_FRAMES_REFERENCE="${PROJECT_SOURCE_DIR}/tests/reference/propagators/hpop_covariance_frames.txt"
    ORBELLE_HPOP_COVARIANCE_REFERENCE="${PROJECT_SOURCE_DIR}/tests/reference/propagators/hpop_covariance.txt"
    ORBELLE_HPOP_VARIATIONAL_REFERENCE="${PROJECT_SOURCE_DIR}/tests/reference/propagators/hpop_variational.txt"
    ORBELLE_HPOP_FORCE_REFERENCE="${PROJECT_SOURCE_DIR}/tests/reference/propagators/hpop_force.txt"
    ORBELLE_SRP_UNION_DIFFERENTIAL_REFERENCE="${PROJECT_SOURCE_DIR}/tests/reference/propagators/srp_union_differential.txt"
    ORBELLE_SRP_DIFFERENTIAL_HIGH_REFERENCE="${PROJECT_SOURCE_DIR}/tests/reference/propagators/srp_differential_high.txt"
    ORBELLE_SRP_DIFFERENTIAL_REFERENCE="${PROJECT_SOURCE_DIR}/tests/reference/propagators/srp_differential.txt"
    ORBELLE_DRAG_DIFFERENTIAL_REFERENCE="${PROJECT_SOURCE_DIR}/tests/reference/propagators/drag_differential.txt"
    ORBELLE_DRAG_DENSITY_REFERENCE="${PROJECT_SOURCE_DIR}/tests/reference/propagators/drag_density_differential.txt"
    ORBELLE_DRAG_DENSITY_HIGH_REFERENCE="${PROJECT_SOURCE_DIR}/tests/reference/propagators/drag_density_high.txt"
    ORBELLE_FORCE_DIFFERENTIAL_REFERENCE="${PROJECT_SOURCE_DIR}/tests/reference/propagators/force_differential.txt"
    ORBELLE_GRAVITY_DIFFERENTIAL_REFERENCE="${PROJECT_SOURCE_DIR}/tests/reference/propagators/gravity_differential.txt"
    ORBELLE_GRAVITY_DIFFERENTIAL_POLE_REFERENCE="${PROJECT_SOURCE_DIR}/tests/reference/propagators/gravity_differential_poles.txt"
    ORBELLE_UNIVERSAL_KEPLER_STM_REFERENCE="${PROJECT_SOURCE_DIR}/tests/reference/propagators/universal_kepler_stm.txt"
    ORBELLE_VOP_INTERPOLATION_REFERENCE="${PROJECT_SOURCE_DIR}/tests/reference/propagators/vop_interpolation.txt"
    ORBELLE_UNIVERSAL_KEPLER_REFERENCE="${PROJECT_SOURCE_DIR}/tests/reference/propagators/universal_kepler.txt"
    ORBELLE_SRP_BOUNDARY_REFERENCE="${PROJECT_SOURCE_DIR}/tests/reference/propagators/srp_boundaries.txt"
    ORBELLE_SUNDMAN_REFERENCE="${PROJECT_SOURCE_DIR}/tests/reference/propagators/sundman.txt"
    ORBELLE_INTERPOLATION_REFERENCE=\"${PROJECT_SOURCE_DIR}/tests/reference/propagators/orbit_interpolation.txt\"
    ORBELLE_LONG_STEP_REFERENCE="${PROJECT_SOURCE_DIR}/tests/reference/propagators/long_steps.txt"
    ORBELLE_BS_REFERENCE="${PROJECT_SOURCE_DIR}/tests/reference/propagators/bulirsch_stoer.txt"
    ORBELLE_RKV89_REFERENCE="${PROJECT_SOURCE_DIR}/tests/reference/propagators/rkv89.txt"
    ORBELLE_GRAVITY_FULL_REFERENCE="${PROJECT_SOURCE_DIR}/tests/reference/propagators/gravity_full.txt"
    ORBELLE_RELATIVITY_REFERENCE="${PROJECT_SOURCE_DIR}/tests/reference/propagators/relativity.txt"
    ORBELLE_LOVE_TIDE_REFERENCE="${PROJECT_SOURCE_DIR}/tests/reference/propagators/solid_tides_love.txt"
    ORBELLE_TIME_TIDE_REFERENCE="${PROJECT_SOURCE_DIR}/tests/reference/propagators/solid_tides_time_dependent.txt"
    ORBELLE_SOLID_TIDE_REFERENCE="${PROJECT_SOURCE_DIR}/tests/reference/propagators/solid_tides.txt"
    ORBELLE_GRAVITY_REFERENCE="${PROJECT_SOURCE_DIR}/tests/reference/propagators/gravity.txt"
    ORBELLE_THIRD_BODY_REFERENCE="${PROJECT_SOURCE_DIR}/tests/reference/propagators/third_body.txt"
    ORBELLE_SRP_REFERENCE="${PROJECT_SOURCE_DIR}/tests/reference/propagators/srp.txt"
    ORBELLE_SRP_MULTI_TRAJECTORIES="${PROJECT_SOURCE_DIR}/tests/reference/propagators/srp_multi_trajectories.txt"
    ORBELLE_SRP_MULTI_REFERENCE="${PROJECT_SOURCE_DIR}/tests/reference/propagators/srp_multi_geometry.txt"
    ORBELLE_SRP_GEOMETRY_REFERENCE="${PROJECT_SOURCE_DIR}/tests/reference/propagators/srp_geometry.txt"
    ORBELLE_NRLMSISE_AP_REFERENCE="${PROJECT_SOURCE_DIR}/tests/reference/propagators/nrlmsise00_ap.txt"
    ORBELLE_NRLMSISE_HIGH_REFERENCE="${PROJECT_SOURCE_DIR}/tests/reference/propagators/nrlmsise00_high.txt"
    ORBELLE_NRLMSISE_REFERENCE="${PROJECT_SOURCE_DIR}/tests/reference/propagators/nrlmsise00.txt"
    ORBELLE_JR_REFERENCE="${PROJECT_SOURCE_DIR}/tests/reference/propagators/jacchia_roberts_precise.txt"
    ORBELLE_MASS_REFERENCE="${PROJECT_SOURCE_DIR}/tests/reference/propagators/mass.txt"
    ORBELLE_DRAG_REFERENCE="${PROJECT_SOURCE_DIR}/tests/reference/propagators/drag.txt"
    ORBELLE_HPOP_REFERENCE="${PROJECT_SOURCE_DIR}/tests/reference/propagators/hpop.txt")
target_sources(orbelle_core_tests PRIVATE orbit_design_test.cpp sensor_test.cpp)
target_link_libraries(orbelle_core_tests PRIVATE Orbelle::Core GTest::gtest_main)
orbelle_warnings(orbelle_core_tests)
orbelle_copy_runtime_dlls(orbelle_core_tests)
gtest_discover_tests(orbelle_core_tests DISCOVERY_MODE PRE_TEST TEST_PREFIX "core.")

target_include_directories(orbelle_core_tests PRIVATE "${PROJECT_SOURCE_DIR}/tests/support")
