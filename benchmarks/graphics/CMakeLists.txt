add_library(orbelle_graphics_workload STATIC workload.cpp)
target_include_directories(orbelle_graphics_workload PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}")
target_link_libraries(orbelle_graphics_workload PUBLIC Orbelle::Visualization)
orbelle_warnings(orbelle_graphics_workload)
if(BUILD_TESTING)
    add_executable(orbelle_graphics_workload_tests workload_test.cpp)
    target_link_libraries(orbelle_graphics_workload_tests PRIVATE
        orbelle_graphics_workload Orbelle::Runtime GTest::gtest_main)
    target_compile_definitions(orbelle_graphics_workload_tests PRIVATE
        ORBELLE_BENCHMARK_EOP="${PROJECT_SOURCE_DIR}/data/samples/eop/iers_c04_2000_01_01_2000_01_02/manifest.json")
    orbelle_warnings(orbelle_graphics_workload_tests)
    orbelle_copy_runtime_dlls(orbelle_graphics_workload_tests)
    gtest_discover_tests(orbelle_graphics_workload_tests DISCOVERY_MODE PRE_TEST
        TEST_PREFIX "graphics_workload.")
    find_package(Python3 3.10 COMPONENTS Interpreter QUIET)
    if(Python3_Interpreter_FOUND)
        add_test(NAME graphics_timing.analyzer
            COMMAND "${Python3_EXECUTABLE}" "${PROJECT_SOURCE_DIR}/tools/validation/test_vulkan_timing.py")
    endif()
endif()
