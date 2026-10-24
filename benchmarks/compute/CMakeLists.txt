find_package(nlohmann_json 3.12.0 EXACT CONFIG REQUIRED)
add_executable(orbelle_compute_benchmark main.cpp)
target_link_libraries(orbelle_compute_benchmark PRIVATE
    Orbelle::Runtime nlohmann_json::nlohmann_json)
target_compile_definitions(orbelle_compute_benchmark PRIVATE
    ORBELLE_BENCHMARK_CONFIG="$<CONFIG>"
    ORBELLE_BENCHMARK_COMPILER="${CMAKE_CXX_COMPILER_ID} ${CMAKE_CXX_COMPILER_VERSION}"
    ORBELLE_BENCHMARK_SANITIZERS=$<BOOL:${ORBELLE_ENABLE_SANITIZERS}>)
if(WIN32)
    target_compile_definitions(orbelle_compute_benchmark PRIVATE NOMINMAX WIN32_LEAN_AND_MEAN)
    target_link_libraries(orbelle_compute_benchmark PRIVATE Psapi)
elseif(NOT CMAKE_SYSTEM_NAME STREQUAL "Linux")
    message(FATAL_ERROR "Computation benchmark resource metrics support Windows and Linux")
endif()
orbelle_warnings(orbelle_compute_benchmark)
orbelle_sanitizers(orbelle_compute_benchmark)
orbelle_copy_runtime_dlls(orbelle_compute_benchmark)

add_executable(orbelle_algorithm_benchmark EXCLUDE_FROM_ALL algorithms.cpp)
target_link_libraries(orbelle_algorithm_benchmark PRIVATE
    Orbelle::Runtime nlohmann_json::nlohmann_json)
target_compile_definitions(orbelle_algorithm_benchmark PRIVATE
    ORBELLE_BENCHMARK_CONFIG="$<CONFIG>")
orbelle_warnings(orbelle_algorithm_benchmark)
orbelle_sanitizers(orbelle_algorithm_benchmark)
orbelle_copy_runtime_dlls(orbelle_algorithm_benchmark)

add_executable(orbelle_hpop_benchmark EXCLUDE_FROM_ALL hpop.cpp)
target_link_libraries(orbelle_hpop_benchmark PRIVATE
    Orbelle::Runtime nlohmann_json::nlohmann_json)
target_compile_definitions(orbelle_hpop_benchmark PRIVATE
    ORBELLE_BENCHMARK_CONFIG="$<CONFIG>")
orbelle_warnings(orbelle_hpop_benchmark)
orbelle_sanitizers(orbelle_hpop_benchmark)
orbelle_copy_runtime_dlls(orbelle_hpop_benchmark)

add_executable(orbelle_hpop_reference EXCLUDE_FROM_ALL hpop_reference.cpp)
target_link_libraries(orbelle_hpop_reference PRIVATE
    Orbelle::Runtime nlohmann_json::nlohmann_json)
target_compile_definitions(orbelle_hpop_reference PRIVATE
    ORBELLE_BENCHMARK_CONFIG="$<CONFIG>")
orbelle_warnings(orbelle_hpop_reference)
orbelle_sanitizers(orbelle_hpop_reference)
orbelle_copy_runtime_dlls(orbelle_hpop_reference)

add_executable(orbelle_workflow_benchmark EXCLUDE_FROM_ALL workflows.cpp)
target_link_libraries(orbelle_workflow_benchmark PRIVATE
    Orbelle::Runtime nlohmann_json::nlohmann_json)
target_compile_definitions(orbelle_workflow_benchmark PRIVATE
    ORBELLE_BENCHMARK_CONFIG="$<CONFIG>")
orbelle_warnings(orbelle_workflow_benchmark)
orbelle_sanitizers(orbelle_workflow_benchmark)
orbelle_copy_runtime_dlls(orbelle_workflow_benchmark)

if(BUILD_TESTING)
    find_package(Python3 3.10 COMPONENTS Interpreter REQUIRED)
    add_test(NAME compute_benchmark.contract
        COMMAND "${Python3_EXECUTABLE}" "${PROJECT_SOURCE_DIR}/tools/validation/test_compute_benchmark.py"
            --executable "$<TARGET_FILE:orbelle_compute_benchmark>"
            --source "${PROJECT_SOURCE_DIR}" --work "${CMAKE_CURRENT_BINARY_DIR}/contract")
    set_tests_properties(compute_benchmark.contract PROPERTIES TIMEOUT 120)
endif()
