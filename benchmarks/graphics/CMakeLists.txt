add_library(orbitforge_graphics_workload STATIC workload.cpp)
target_include_directories(orbitforge_graphics_workload PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}")
target_link_libraries(orbitforge_graphics_workload PUBLIC OrbitForge::Visualization)
orbitforge_warnings(orbitforge_graphics_workload)
if(BUILD_TESTING)
    add_executable(orbitforge_graphics_workload_tests workload_test.cpp)
    target_link_libraries(orbitforge_graphics_workload_tests PRIVATE
        orbitforge_graphics_workload OrbitForge::Runtime GTest::gtest_main)
    target_compile_definitions(orbitforge_graphics_workload_tests PRIVATE
        ORBITFORGE_BENCHMARK_EOP="${PROJECT_SOURCE_DIR}/data/samples/eop/iers_c04_2000_01_01_2000_01_02/manifest.json")
    orbitforge_warnings(orbitforge_graphics_workload_tests)
    orbitforge_copy_runtime_dlls(orbitforge_graphics_workload_tests)
    gtest_discover_tests(orbitforge_graphics_workload_tests DISCOVERY_MODE PRE_TEST
        TEST_PREFIX "graphics_workload.")
    find_package(Python3 3.10 COMPONENTS Interpreter QUIET)
    if(Python3_Interpreter_FOUND)
        add_test(NAME graphics_timing.analyzer
            COMMAND "${Python3_EXECUTABLE}" "${PROJECT_SOURCE_DIR}/tools/validation/test_vulkan_timing.py")
    endif()
endif()
