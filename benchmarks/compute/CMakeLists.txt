find_package(nlohmann_json 3.12.0 EXACT CONFIG REQUIRED)
add_executable(orbitforge_compute_benchmark main.cpp)
target_link_libraries(orbitforge_compute_benchmark PRIVATE
    OrbitForge::Runtime nlohmann_json::nlohmann_json)
target_compile_definitions(orbitforge_compute_benchmark PRIVATE
    ORBITFORGE_BENCHMARK_CONFIG="$<CONFIG>"
    ORBITFORGE_BENCHMARK_COMPILER="${CMAKE_CXX_COMPILER_ID} ${CMAKE_CXX_COMPILER_VERSION}"
    ORBITFORGE_BENCHMARK_SANITIZERS=$<BOOL:${ORBITFORGE_ENABLE_SANITIZERS}>)
if(WIN32)
    target_compile_definitions(orbitforge_compute_benchmark PRIVATE NOMINMAX WIN32_LEAN_AND_MEAN)
    target_link_libraries(orbitforge_compute_benchmark PRIVATE Psapi)
elseif(NOT CMAKE_SYSTEM_NAME STREQUAL "Linux")
    message(FATAL_ERROR "Computation benchmark resource metrics support Windows and Linux")
endif()
orbitforge_warnings(orbitforge_compute_benchmark)
orbitforge_sanitizers(orbitforge_compute_benchmark)
orbitforge_copy_runtime_dlls(orbitforge_compute_benchmark)

add_executable(orbitforge_workflow_benchmark EXCLUDE_FROM_ALL workflows.cpp)
target_link_libraries(orbitforge_workflow_benchmark PRIVATE
    OrbitForge::Runtime nlohmann_json::nlohmann_json)
target_compile_definitions(orbitforge_workflow_benchmark PRIVATE
    ORBITFORGE_BENCHMARK_CONFIG="$<CONFIG>")
orbitforge_warnings(orbitforge_workflow_benchmark)
orbitforge_sanitizers(orbitforge_workflow_benchmark)
orbitforge_copy_runtime_dlls(orbitforge_workflow_benchmark)

if(BUILD_TESTING)
    find_package(Python3 3.10 COMPONENTS Interpreter REQUIRED)
    add_test(NAME compute_benchmark.contract
        COMMAND "${Python3_EXECUTABLE}" "${PROJECT_SOURCE_DIR}/tools/validation/test_compute_benchmark.py"
            --executable "$<TARGET_FILE:orbitforge_compute_benchmark>"
            --source "${PROJECT_SOURCE_DIR}" --work "${CMAKE_CURRENT_BINARY_DIR}/contract")
    set_tests_properties(compute_benchmark.contract PROPERTIES TIMEOUT 120)
endif()
