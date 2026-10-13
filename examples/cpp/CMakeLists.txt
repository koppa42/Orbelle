add_executable(orbitforge_example circular_orbit.cpp)
target_link_libraries(orbitforge_example PRIVATE OrbitForge::Runtime)
orbitforge_warnings(orbitforge_example)

# The project example propagates a general two-body orbit (CSPICE prop2b).
if(ORBITFORGE_WITH_SPICE)
    add_executable(orbitforge_project_example project_analysis.cpp)
    target_link_libraries(orbitforge_project_example PRIVATE OrbitForge::Runtime)
    orbitforge_warnings(orbitforge_project_example)
    orbitforge_copy_runtime_dlls(orbitforge_project_example)
    if(BUILD_TESTING)
        add_test(NAME example.project_api COMMAND orbitforge_project_example)
    endif()
endif()

